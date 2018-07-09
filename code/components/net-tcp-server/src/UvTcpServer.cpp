/*
 * This file is part of the CitizenFX project - http://citizen.re/
 *
 * See LICENSE and MENTIONS in the root of the source tree for information
 * regarding licensing.
 */

#include "StdInc.h"
#include "UvTcpServer.h"
#include "TcpServerManager.h"
#include "memdbgon.h"

template<typename Handle, class Class, void(Class::*Callable)()>
void UvCallback(Handle* handle)
{
	(reinterpret_cast<Class*>(handle->data)->*Callable)();
}

template<typename Handle, class Class, typename T1, void(Class::*Callable)(T1)>
void UvCallback(Handle* handle, T1 a1)
{
	(reinterpret_cast<Class*>(handle->data)->*Callable)(a1);
}

template<typename Handle, class Class, typename T1, typename T2, void(Class::*Callable)(T1, T2)>
void UvCallback(Handle* handle, T1 a1, T2 a2)
{
	(reinterpret_cast<Class*>(handle->data)->*Callable)(a1, a2);
}

namespace net
{
UvTcpServer::UvTcpServer(TcpServerManager* manager)
	: m_manager(manager)
{

}

UvTcpServer::~UvTcpServer()
{
	m_clients.clear();

	if (m_server.get())
	{
		UvClose(std::move(m_server));
	}
}

bool UvTcpServer::Listen(std::unique_ptr<uv_tcp_t>&& server)
{
	m_server = std::move(server);

	int result = uv_listen(reinterpret_cast<uv_stream_t*>(m_server.get()), 0, UvCallback<uv_stream_t, UvTcpServer, int, &UvTcpServer::OnConnection>);

	bool retval = (result == 0);

	if (!retval)
	{
		trace("Listening on socket failed - libuv error %s.\n", uv_strerror(result));
	}

	return retval;
}

void UvTcpServer::OnConnection(int status)
{
	// check for error conditions
	if (status < 0)
	{
		trace("error on connection: %s\n", uv_strerror(status));
		return;
	}

	// initialize a handle for the client
	std::unique_ptr<uv_tcp_t> clientHandle = std::make_unique<uv_tcp_t>();
	uv_tcp_init(m_manager->GetLoop(), clientHandle.get());

	// create a stream instance and associate
	fwRefContainer<UvTcpServerStream> stream(new UvTcpServerStream(this));
	clientHandle->data = stream.GetRef();

	// attempt accepting the connection
	if (stream->Accept(std::move(clientHandle)))
	{
		m_clients.insert(stream);
		
		// invoke the connection callback
		if (GetConnectionCallback())
		{
			GetConnectionCallback()(stream);
		}
	}
	else
	{
		stream = nullptr;
	}
}

void UvTcpServer::RemoveStream(UvTcpServerStream* stream)
{
	m_clients.erase(stream);
}

UvTcpServerStream::UvTcpServerStream(UvTcpServer* server)
	: m_server(server)
{

}

UvTcpServerStream::~UvTcpServerStream()
{
	CloseClient();
}

void UvTcpServerStream::CloseClient()
{
	if (m_client.get())
	{
		uv_read_stop(reinterpret_cast<uv_stream_t*>(m_client.get()));

		UvClose(std::move(m_client));
		UvClose(std::move(m_writeCallback));
	}
}

bool UvTcpServerStream::Accept(std::unique_ptr<uv_tcp_t>&& client)
{
	m_client = std::move(client);

	uv_tcp_nodelay(m_client.get(), true);

	// initialize a write callback handle
	m_writeCallback = std::make_unique<uv_async_t>();
	m_writeCallback->data = this;

	uv_async_init(m_server->GetManager()->GetLoop(), m_writeCallback.get(), UvCallback<uv_async_t, UvTcpServerStream, &UvTcpServerStream::HandlePendingWrites>);

	// accept
	int result = uv_accept(reinterpret_cast<uv_stream_t*>(m_server->GetServer()),
						   reinterpret_cast<uv_stream_t*>(m_client.get()));

	if (result == 0)
	{
		uv_read_start(reinterpret_cast<uv_stream_t*>(m_client.get()), [] (uv_handle_t* handle, size_t suggestedSize, uv_buf_t* buf)
		{
			UvTcpServerStream* stream = reinterpret_cast<UvTcpServerStream*>(handle->data);

			auto& readBuffer = stream->GetReadBuffer();
			readBuffer.resize(suggestedSize);

			buf->base = &readBuffer[0];
			buf->len = suggestedSize;
		}, UvCallback<uv_stream_t, UvTcpServerStream, ssize_t, const uv_buf_t*, &UvTcpServerStream::HandleRead>);
	}

	return (result == 0);
}

void UvTcpServerStream::HandleRead(ssize_t nread, const uv_buf_t* buf)
{
	if (nread > 0)
	{
		std::vector<uint8_t> targetBuf(nread);
		memcpy(&targetBuf[0], buf->base, targetBuf.size());

		if (GetReadCallback())
		{
			GetReadCallback()(targetBuf);
		}
	}
	else if (nread < 0)
	{
		// hold a reference to ourselves while in this scope
		fwRefContainer<UvTcpServerStream> tempContainer = this;

		Close();
	}
}

PeerAddress UvTcpServerStream::GetPeerAddress()
{
	sockaddr_storage addr;
	int len = sizeof(addr);

	uv_tcp_getpeername(m_client.get(), reinterpret_cast<sockaddr*>(&addr), &len);

	return PeerAddress(reinterpret_cast<sockaddr*>(&addr), static_cast<socklen_t>(len));
}

void UvTcpServerStream::Write(const std::vector<uint8_t>& data)
{
	if (!m_client)
	{
		return;
	}

	// write request structure
	struct UvWriteReq
	{
		std::vector<uint8_t> sendData;
		uv_buf_t buffer;
		uv_write_t write;

		fwRefContainer<UvTcpServerStream> stream;
	};

	// prepare a write request
	UvWriteReq* writeReq = new UvWriteReq;
	writeReq->sendData = data;
	writeReq->buffer.base = reinterpret_cast<char*>(&writeReq->sendData[0]);
	writeReq->buffer.len = writeReq->sendData.size();
	writeReq->stream = this;
	
	writeReq->write.data = writeReq;

	// submit the write request
	m_pendingRequests.push([=]()
	{
		if (!m_client)
		{
			return;
		}

		// send the write request
		uv_write(&writeReq->write, reinterpret_cast<uv_stream_t*>(m_client.get()), &writeReq->buffer, 1, [](uv_write_t* write, int status)
		{
			UvWriteReq* req = reinterpret_cast<UvWriteReq*>(write->data);

			if (status < 0)
			{
				//trace("write to %s failed - %s\n", req->stream->GetPeerAddress().ToString().c_str(), uv_strerror(status));
			}

			delete req;
		});
	});

	// wake the callback
	uv_async_send(m_writeCallback.get());
}

void UvTcpServerStream::HandlePendingWrites()
{
	if (!m_client)
	{
		return;
	}

	// as a possible result is closing self, keep reference
	fwRefContainer<UvTcpServerStream> selfRef = this;

	// dequeue pending writes
	std::function<void()> request;

	while (m_pendingRequests.try_pop(request))
	{
		request();
	}
}

void UvTcpServerStream::Close()
{
	m_pendingRequests.push([=]()
	{
		// keep a reference in scope
		fwRefContainer<UvTcpServerStream> selfRef = this;

		CloseClient();

		SetReadCallback(TReadCallback());

		// get it locally as we may recurse
		auto closeCallback = GetCloseCallback();

		if (closeCallback)
		{
			SetCloseCallback(TCloseCallback());

			closeCallback();
		}

		m_server->RemoveStream(this);
	});

	// wake the callback
	uv_async_send(m_writeCallback.get());
}
}
