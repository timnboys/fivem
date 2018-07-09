/*
 * This file is part of the CitizenFX project - http://citizen.re/
 *
 * See LICENSE and MENTIONS in the root of the source tree for information
 * regarding licensing.
 */

#include "StdInc.h"
#include "MumbleClientImpl.h"
#include <thread>
#include <chrono>

static __declspec(thread) MumbleClient* g_currentMumbleClient;

inline std::chrono::milliseconds msec()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now().time_since_epoch());
}

void MumbleClient::Initialize()
{
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);

	m_beginConnectEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	m_socketConnectEvent = nullptr;
	m_socketReadEvent = nullptr;
	m_idleEvent = CreateWaitableTimer(nullptr, FALSE, nullptr);

	m_mumbleThread = std::thread(ThreadFunc, this);

	m_audioInput.Initialize();
	m_audioInput.SetClient(this);

	m_audioOutput.Initialize();
	m_audioOutput.SetClient(this);

	WSADATA wsaData;

	WSAStartup(MAKEWORD(2, 2), &wsaData);
}

concurrency::task<MumbleConnectionInfo*> MumbleClient::ConnectAsync(const net::PeerAddress& address, const std::string& userName)
{
	m_connectionInfo.address = address;
	m_connectionInfo.username = userName;

	m_tcpPingAverage = 0.0f;
	m_tcpPingVariance = 0.0f;

	m_tcpPingCount = 0;

	memset(m_tcpPings, 0, sizeof(m_tcpPings));

	m_state.SetClient(this);
	m_state.SetUsername(ToWide(userName));

	SetEvent(m_beginConnectEvent);

	m_completionEvent = concurrency::task_completion_event<MumbleConnectionInfo*>();

	return concurrency::task<MumbleConnectionInfo*>(m_completionEvent);
}

concurrency::task<void> MumbleClient::DisconnectAsync()
{
	if (m_tlsClient)
	{
		m_tlsClient->close();
	}

	m_connectionInfo = {};

	return concurrency::task_from_result();
}

void MumbleClient::SetActivationMode(MumbleActivationMode mode)
{
	return m_audioInput.SetActivationMode(mode);
}

void MumbleClient::SetActivationLikelihood(MumbleVoiceLikelihood likelihood)
{
	return m_audioInput.SetActivationLikelihood(likelihood);
}

void MumbleClient::SetInputDevice(const std::string& dsoundDeviceId)
{
	return m_audioInput.SetAudioDevice(dsoundDeviceId);
}

void MumbleClient::SetOutputDevice(const std::string& dsoundDeviceId)
{
	return m_audioOutput.SetAudioDevice(dsoundDeviceId);
}

void MumbleClient::SetPTTButtonState(bool pressed)
{
	return m_audioInput.SetPTTButtonState(pressed);
}

void MumbleClient::SetOutputVolume(float volume)
{
	return m_audioOutput.SetVolume(volume);
}

void MumbleClient::SetChannel(const std::string& channelName)
{
	if (!m_connectionInfo.isConnected)
	{
		return;
	}

	if (channelName == m_curManualChannel)
	{
		return;
	}

	m_curManualChannel = channelName;

	// check if the channel already exists
	std::wstring wname = ToWide(channelName);

	for (const auto& channel : m_state.GetChannels())
	{
		if (channel.second.GetName() == wname)
		{
			// join the channel
			MumbleProto::UserState state;
			state.set_session(m_state.GetSession());
			state.set_channel_id(channel.first);

			Send(MumbleMessageType::UserState, state);
			return;
		}
	}

	// it does not, create the channel
	{
		MumbleProto::ChannelState chan;
		chan.set_parent(0);
		chan.set_name(channelName);
		chan.set_temporary(true);

		Send(MumbleMessageType::ChannelState, chan);
	}
}

void MumbleClient::SetAudioDistance(float distance)
{
	m_audioOutput.SetDistance(distance);
}

float MumbleClient::GetInputAudioLevel()
{
	return m_audioInput.GetAudioLevel();
}

void MumbleClient::GetTalkers(std::vector<std::string>* referenceIds)
{
	referenceIds->clear();

	std::vector<uint32_t> sessions;
	m_audioOutput.GetTalkers(&sessions);

	for (uint32_t session : sessions)
	{
		auto user = m_state.GetUser(session);

		referenceIds->push_back(ToNarrow(user->GetName()));
	}

	// local talker talking?
	if (m_audioInput.IsTalking())
	{
		referenceIds->push_back(ToNarrow(m_state.GetUsername()));
	}
}

bool MumbleClient::IsAnyoneTalking()
{
	std::vector<uint32_t> talkers;
	m_audioOutput.GetTalkers(&talkers);

	return (!talkers.empty());
}

void MumbleClient::SetActorPosition(float position[3])
{
	m_audioInput.SetPosition(position);
}

void MumbleClient::SetListenerMatrix(float position[3], float front[3], float up[3])
{
	m_audioOutput.SetMatrix(position, front, up);
}

void MumbleClient::ThreadFuncImpl()
{
	SetThreadName(-1, "[Mumble] Network Thread");

	while (true)
	{
		ClientTask task = WaitForTask();

		try
		{
			switch (task)
			{
				case ClientTask::BeginConnect:
				{
					const auto& address = m_connectionInfo.address;

					m_socket = socket(address.GetAddressFamily(), SOCK_STREAM, IPPROTO_TCP);

					int on = 1;
					setsockopt(m_socket, IPPROTO_TCP, TCP_NODELAY, (char*)&on, sizeof(on));

					m_socketConnectEvent = WSACreateEvent();
					m_socketReadEvent = WSACreateEvent();

					WSAEventSelect(m_socket, m_socketConnectEvent, FD_CONNECT);

					u_long nonBlocking = 1;
					ioctlsocket(m_socket, FIONBIO, &nonBlocking);

					connect(m_socket, address.GetSocketAddress(), address.GetSocketAddressLength());

					trace("[mumble] connecting to %s...\n", address.ToString());

					break;
				}

				case ClientTask::EndConnect:
				{
					WSANETWORKEVENTS events = { 0 };
					WSAEnumNetworkEvents(m_socket, m_socketConnectEvent, &events);

					if (events.iErrorCode[FD_CONNECT_BIT])
					{
						// TODO: reconnecting?
						trace("[mumble] connecting failed: %d\n", events.iErrorCode[FD_CONNECT_BIT]);

						m_completionEvent.set_exception(std::runtime_error("Failed Mumble connection."));

						break;
					}

					WSACloseEvent(m_socketConnectEvent);
					m_socketConnectEvent = INVALID_HANDLE_VALUE;

					LARGE_INTEGER waitTime;
					waitTime.QuadPart = -20000000LL;

					SetWaitableTimer(m_idleEvent, &waitTime, 0, nullptr, nullptr, 0);

					m_handler.Reset();

					WSAEventSelect(m_socket, m_socketReadEvent, FD_READ);

					m_sessionManager = std::make_unique<Botan::TLS::Session_Manager_In_Memory>(m_rng);

					m_credentials = std::make_unique<MumbleCredentialsManager>();

					m_tlsClient = std::make_shared<Botan::TLS::Client>(*this,
																	   *(m_sessionManager.get()),
																	   *(m_credentials.get()),
																	   m_policy,
																	   m_rng,
																	   Botan::TLS::Server_Information()
																	   );

					m_connectionInfo.isConnected = true;

					break;
				}

				case ClientTask::Idle:
				{
					if (m_tlsClient->is_active() && m_connectionInfo.isConnected)
					{
						MumbleProto::Ping ping;
						ping.set_timestamp(msec().count());
						ping.set_tcp_ping_avg(m_tcpPingAverage);
						ping.set_tcp_ping_var(m_tcpPingVariance);
						ping.set_tcp_packets(m_tcpPingCount);

						Send(MumbleMessageType::Ping, ping);

						LARGE_INTEGER waitTime;
						waitTime.QuadPart = -50000000LL;

						SetWaitableTimer(m_idleEvent, &waitTime, 0, nullptr, nullptr, 0);
					}

					break;
				}

				case ClientTask::RecvData:
				{
					WSANETWORKEVENTS ne;
					WSAEnumNetworkEvents(m_socket, m_socketReadEvent, &ne);

					uint8_t buffer[16384];
					int len = recv(m_socket, (char*)buffer, sizeof(buffer), 0);

					if (len > 0)
					{
						std::unique_lock<std::recursive_mutex> lock(m_clientMutex);
						m_tlsClient->received_data(buffer, len);
					}
					else if (len == 0)
					{
						// TCP close, graceful?
						trace("[mumble] tcp close :(\n");

						// try to reconnect
						closesocket(m_socket);

						SetEvent(m_beginConnectEvent);
					}
					else
					{
						if (WSAGetLastError() != WSAEWOULDBLOCK)
						{
							// TCP close, error state
							trace("[mumble] tcp error :(\n");

							// try to reconnect
							closesocket(m_socket);

							SetEvent(m_beginConnectEvent);
						}
					}

					break;
				}

			}
		}
		catch (std::exception& e)
		{
			trace("lolexception %s", e.what());
		}
	}
}

void MumbleClient::MarkConnected()
{
	m_completionEvent.set(&m_connectionInfo);
}

MumbleConnectionInfo* MumbleClient::GetConnectionInfo()
{
	return &m_connectionInfo;
}

void MumbleClient::HandlePing(const MumbleProto::Ping& ping)
{
	m_tcpPingCount++;

	if (ping.has_timestamp())
	{
		// time delta
		auto timeDelta = msec().count() - ping.timestamp();

		// which ping this is in the history list
		size_t thisPing = m_tcpPingCount - 1;

		// move pings down
		if (thisPing >= _countof(m_tcpPings))
		{
			for (size_t i = 1; i < _countof(m_tcpPings); i++)
			{
				m_tcpPings[i - 1] = m_tcpPings[i];
			}

			thisPing = _countof(m_tcpPings) - 1;
		}

		// store this ping
		m_tcpPings[thisPing] = timeDelta;

		// calculate average
		uint32_t avgCount = 0;

		for (size_t i = 0; i < thisPing; i++)
		{
			avgCount += m_tcpPings[i];
		}

		m_tcpPingAverage = avgCount / float(thisPing + 1);

		// calculate variance
		float varianceCount = 0;

		for (size_t i = 0; i < thisPing; i++)
		{
			auto var = float(m_tcpPings[i]) - m_tcpPingAverage;
			varianceCount += var;
		}

		m_tcpPingVariance = varianceCount / (thisPing + 1);
	}
}

void MumbleClient::Send(MumbleMessageType type, const char* buf, size_t size)
{
	MumblePacketHeader header;
	header.SetPacketType((uint16_t)type);
	header.SetPacketLength(size);

	Send((const char*)&header, sizeof(header));
	Send(buf, size);
}

void MumbleClient::Send(const char* buf, size_t size)
{
	if (!m_connectionInfo.isConnected)
	{
		return;
	}

	std::unique_lock<std::recursive_mutex> lock(m_clientMutex);

	if (m_tlsClient->is_active())
	{
		m_tlsClient->send((const uint8_t*)buf, size);
	}
}

void MumbleClient::WriteToSocket(const uint8_t buf[], size_t length)
{
	send(m_socket, (const char*)buf, length, 0);
}

void MumbleClient::OnAlert(Botan::TLS::Alert alert, const uint8_t[], size_t)
{
	trace("[mumble] TLS alert: %s\n", alert.type_string().c_str());

	if (alert.is_fatal())
	{
		closesocket(m_socket);

		m_connectionInfo.isConnected = false;
	}
}

void MumbleClient::OnReceive(const uint8_t buf[], size_t length)
{
	g_currentMumbleClient = this;

	m_handler.HandleIncomingData(buf, length);
}

bool MumbleClient::OnHandshake(const Botan::TLS::Session& session)
{
	trace("[mumble] got session %s %s\n", session.version().to_string().c_str(), session.ciphersuite().to_string().c_str());

	return true;
}

void MumbleClient::OnActivated()
{
	// send our own version
	MumbleProto::Version ourVersion;
	ourVersion.set_version(0x00010204);
	ourVersion.set_os("Windows");
	ourVersion.set_os_version("Cfx/Embedded");
	ourVersion.set_release("CitizenFX Client");

	this->Send(MumbleMessageType::Version, ourVersion);
}

fwRefContainer<MumbleClient> MumbleClient::GetCurrent()
{
	return g_currentMumbleClient;
}

ClientTask MumbleClient::WaitForTask()
{
	HANDLE waitHandles[16];
	int waitCount = 0;

	waitHandles[waitCount] = m_beginConnectEvent;
	waitCount++;

	if (m_socketConnectEvent && m_socketConnectEvent != INVALID_HANDLE_VALUE)
	{
		waitHandles[waitCount] = m_socketConnectEvent;
		waitCount++;
	}

	if (m_socketReadEvent && m_socketReadEvent != INVALID_HANDLE_VALUE)
	{
		waitHandles[waitCount] = m_socketReadEvent;
		waitCount++;
	}

	waitHandles[waitCount] = m_idleEvent;
	waitCount++;

	DWORD waitResult = WSAWaitForMultipleEvents(waitCount, waitHandles, FALSE, INFINITE, FALSE);

	if (waitResult >= WAIT_OBJECT_0 && waitResult <= (WAIT_OBJECT_0 + waitCount))
	{
		HANDLE compareHandle = waitHandles[waitResult - WAIT_OBJECT_0];

		if (compareHandle == m_beginConnectEvent)
		{
			return ClientTask::BeginConnect;
		}
		else if (compareHandle == m_socketConnectEvent)
		{
			return ClientTask::EndConnect;
		}
		else if (compareHandle == m_socketReadEvent)
		{
			return ClientTask::RecvData;
		}
		else if (compareHandle == m_idleEvent)
		{
			return ClientTask::Idle;
		}
	}
}

void MumbleClient::ThreadFunc(MumbleClient* client)
{
	client->ThreadFuncImpl();
}

fwRefContainer<IMumbleClient> CreateMumbleClient()
{
	return new MumbleClient();
}
