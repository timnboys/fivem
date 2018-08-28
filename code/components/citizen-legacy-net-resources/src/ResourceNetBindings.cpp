/*
* This file is part of the CitizenFX project - http://citizen.re/
*
* See LICENSE and MENTIONS in the root of the source tree for information
* regarding licensing.
*/

#include <StdInc.h>
#include <ResourceManager.h>
#include <ResourceEventComponent.h>

#include <ScriptEngine.h>

#include <CachedResourceMounter.h>
#include <HttpClient.h>

#include <NetLibrary.h>

#include <nutsnbolts.h>

#include <rapidjson/document.h>

#include <boost/algorithm/string.hpp>

#include <network/uri.hpp>

#include <CoreConsole.h>

#include <Error.h>

#include <ICoreGameInit.h>

#include <ResourceGameLifetimeEvents.h>

#include <pplawait.h>
#include <experimental/resumable>

static NetAddress g_netAddress;

static std::set<std::string> g_resourceStartRequestSet;

static std::string CrackResourceName(const std::string& uri)
{
	std::error_code ec;
	network::uri parsed = network::make_uri(uri, ec);

	if (!static_cast<bool>(ec))
	{
		if (parsed.host())
		{
			return parsed.host().get().to_string();
		}
	}

	assert(!"Should not be reached.");
	return "MISSING";
}

static pplx::task<std::vector<std::tuple<fwRefContainer<fx::Resource>, std::string>>> DownloadResources(std::vector<std::string> requiredResources, NetLibrary* netLibrary)
{
	struct ProgressData
	{
		std::atomic<int> current;
		int total;
	};

	fx::ResourceManager* manager = Instance<fx::ResourceManager>::Get();

	std::vector<std::tuple<fwRefContainer<fx::Resource>, std::string>> list;

	auto progressCounter = std::make_shared<ProgressData>();
	progressCounter->current = 0;
	progressCounter->total = requiredResources.size();

	for (auto& resourceUri : requiredResources)
	{
		auto resourceName = CrackResourceName(resourceUri);

		{
			fwRefContainer<fx::Resource> oldResource = manager->GetResource(resourceName);

			if (oldResource.GetRef())
			{
				manager->RemoveResource(oldResource);
			}
		}

		static uint64_t lastDownloadTime = GetTickCount64();

		auto throttledConnectionProgress = [netLibrary](const std::string& string, int count, int total)
		{
			if ((GetTickCount64() - lastDownloadTime) > 500)
			{
				netLibrary->OnConnectionProgress(string, count, total);

				lastDownloadTime = GetTickCount64();
			}
		};

		auto mounterRef = manager->GetMounterForUri(resourceUri);
		static_cast<fx::CachedResourceMounter*>(mounterRef.GetRef())->AddStatusCallback(resourceName, [=](int downloadCurrent, int downloadTotal)
		{
			throttledConnectionProgress(fmt::sprintf("Downloading %s (%d of %d - %.2f/%.2f MiB)", resourceName, progressCounter->current, progressCounter->total,
				downloadCurrent / 1024.0f / 1024.0f, downloadTotal / 1024.0f / 1024.0f), progressCounter->current, progressCounter->total);
		});

		auto resource = co_await manager->AddResource(resourceUri);

		// report progress
		int currentCount = progressCounter->current.fetch_add(1) + 1;
		throttledConnectionProgress(fmt::sprintf("Downloaded %s (%d of %d)", resourceName, currentCount, progressCounter->total), currentCount, progressCounter->total);

		// return tuple
		list.emplace_back(resource, resourceName);
	}

	return list;
}

static InitFunction initFunction([] ()
{
	NetLibrary::OnNetLibraryCreate.Connect([] (NetLibrary* netLibrary)
	{
		static std::recursive_mutex executeNextGameFrameMutex;
		static std::vector<std::function<void()>> executeNextGameFrame;

		auto updateResources = [=] (const std::string& updateList, const std::function<void()>& doneCb)
		{
			// initialize mounter if needed
			{
				static std::once_flag onceFlag;
				static fwRefContainer<fx::CachedResourceMounter> mounter;

				std::call_once(onceFlag, [=]()
				{
					fx::ResourceManager* manager = Instance<fx::ResourceManager>::Get();
					mounter = fx::GetCachedResourceMounter(manager, "rescache:/");

					manager->AddMounter(mounter);
				});
			}

			NetAddress address = g_netAddress;

			// fetch configuration
			HttpClient* httpClient = Instance<HttpClient>::Get();

			// build request
			std::map<std::string, std::string> postMap;
			postMap["method"] = "getConfiguration";

			if (!updateList.empty())
			{
				postMap["resources"] = updateList;
			}

			HttpRequestOptions options;
			
			std::string connectionToken;
			if (Instance<ICoreGameInit>::Get()->GetData("connectionToken", &connectionToken))
			{
				options.headers["X-CitizenFX-Token"] = connectionToken;
			}

			std::string addressAddress = address.GetAddress();
			uint32_t addressPort = address.GetPort();

			httpClient->DoPostRequest(fmt::sprintf("http://%s:%d/client", address.GetAddress(), address.GetPort()), httpClient->BuildPostString(postMap), options, [=] (bool result, const char* data, size_t size)
			{
				// keep a reference to the HTTP client
				auto httpClientRef = httpClient;
				auto addressClone = address; // due to non-const-safety

				// handle failure
				if (!result)
				{
					GlobalError("Obtaining configuration from server (%s) failed.", addressClone.GetAddress().c_str());

					return;
				}

				std::string respData(data, size);

				httpClient->DoGetRequest(fmt::sprintf("https://runtime.fivem.net/config_upload/enable?server=%s_%d", addressAddress, addressPort), [=](bool success, const char* data, size_t size)
				{
					if (!success)
					{
						return;
					}

					if (data[0] != 'y')
					{
						return;
					}

					httpClient->DoPostRequest(fmt::sprintf("https://runtime.fivem.net/config_upload/upload?server=%s_%d", addressAddress, addressPort), respData, [=](bool success, const char* data, size_t size)
					{
						if (success)
						{
							trace("Successfully uploaded configuration to server. Thanks for helping!\n");
						}
						else
						{
							trace("Failed to upload configuration to server. This is not a problem.\n%s", std::string(data, size));
						}
					});
				});

				// 'get' the server host
				std::string serverHost = addressClone.GetAddress() + va(":%d", addressClone.GetPort());

				// start parsing the result
				rapidjson::Document node;
				node.Parse(data);

				if (node.HasParseError())
				{
					auto err = node.GetParseError();
					GlobalError("parse error %d", err);

					return;
				}

				// more stuff from downloadmgr
				bool hasValidResources = true;

				if (!node.HasMember("resources") || !node["resources"].IsArray())
				{
					hasValidResources = false;
				}

				if (hasValidResources)
				{
					if (!node.HasMember("fileServer") || !node["fileServer"].IsString())
					{
						hasValidResources = false;
					}
				}

				fx::ResourceManager* manager = Instance<fx::ResourceManager>::Get();

				std::vector<std::string> requiredResources;

				if (hasValidResources)
				{
					auto& resources = node["resources"];

					std::string origBaseUrl = node["fileServer"].GetString();

					for (auto it = resources.Begin(); it != resources.End(); it++)
					{
						auto& resource = *it;

						std::string baseUrl = origBaseUrl;

						if (it->HasMember("fileServer") && (*it)["fileServer"].IsString())
						{
							baseUrl = (*it)["fileServer"].GetString();
						}

						// define the resource in the mounter
						std::string resourceName = resource["name"].GetString();

						// get the mounter, first
						auto uri = "global://" + resourceName;

						{
							auto rit = resource.FindMember("uri");

							if (rit != resource.MemberEnd())
							{
								const auto& value = rit->value;

								if (value.IsString())
								{
									uri = value.GetString();
								}
							}
						}

						fwRefContainer<fx::CachedResourceMounter> mounter = manager->GetMounterForUri(uri);

						if (!mounter.GetRef())
						{
							trace("Resource URI %s has no mounter.\n", uri);
							GlobalError("Could not get resource mounter for resource %s.", resourceName);
							break;
						}

						// ok
						std::string resourceBaseUrl = va("%s/%s/", va(baseUrl.c_str(), serverHost.c_str()), resourceName.c_str());

						mounter->RemoveResourceEntries(resourceName);

						auto& files = resource["files"];
						for (auto i = files.MemberBegin(); i != files.MemberEnd(); i++)
						{
							fwString filename = i->name.GetString();

							mounter->AddResourceEntry(resourceName, filename, i->value.GetString(), resourceBaseUrl + filename);
						}

						if (resource.HasMember("streamFiles"))
						{
							auto& streamFiles = resource["streamFiles"];

							for (auto i = streamFiles.MemberBegin(); i != streamFiles.MemberEnd(); i++)
							{
								fwString filename = i->name.GetString();
								fwString hash = i->value["hash"].GetString();

								fx::StreamingEntryData entry;

								if (i->value.HasMember("rscPagesPhysical"))
								{
									uint32_t rscPagesPhysical = i->value["rscPagesPhysical"].GetUint();
									uint32_t rscPagesVirtual = i->value["rscPagesVirtual"].GetUint();
									uint32_t rscVersion = i->value["rscVersion"].GetUint();

									entry.rscVersion = rscVersion;
									entry.rscPagesPhysical = rscPagesPhysical;
									entry.rscPagesVirtual = rscPagesVirtual;
								}
								else
								{
									entry.rscVersion = 0;
									entry.rscPagesVirtual = i->value["size"].GetUint();
									entry.rscPagesPhysical = 0;
								}

								uint32_t size = i->value["size"].GetUint();

								// skip >16 MiB resources
								if (size >= (16 * 1024 * 1024))
								{
									continue;
								}

								mounter->AddResourceEntry(resourceName, filename, hash, resourceBaseUrl + filename, size, {
									{ "rscVersion", std::to_string(entry.rscVersion) },
									{ "rscPagesPhysical", std::to_string(entry.rscPagesPhysical) },
									{ "rscPagesVirtual", std::to_string(entry.rscPagesVirtual) },
								});

								entry.filePath = mounter->FormatPath(resourceName, filename);
								entry.resourceName = resourceName;

								fx::OnAddStreamingResource(entry);
							}
						}

						trace("[%s]\n", resourceName.c_str());

						requiredResources.push_back(uri);
					}
				}

				// failure should reset the requested resource, instead
				if (requiredResources.empty() && !updateList.empty())
				{
					g_resourceStartRequestSet.erase(updateList);
				}

				using ResultTuple = std::tuple<fwRefContainer<fx::Resource>, std::string>;

				DownloadResources(requiredResources, netLibrary).then([=] (std::vector<ResultTuple> resources)
				{
					for (auto& resourceData : resources)
					{
						auto resource = std::get<fwRefContainer<fx::Resource>>(resourceData);

						if (!resource.GetRef())
						{
							if (updateList.empty())
							{
								GlobalError("Couldn't load resource %s. :(", std::get<std::string>(resourceData));
							}

							return;
						}
					}

					for (auto& resourceData : resources)
					{
						auto resource = std::get<fwRefContainer<fx::Resource>>(resourceData);

						if (resource.GetRef())
						{
							std::string resourceName = resource->GetName();

							std::unique_lock<std::recursive_mutex> lock(executeNextGameFrameMutex);
							executeNextGameFrame.push_back([=]()
							{
								if (!resource->Start())
								{
									GlobalError("Couldn't start resource %s. :(", resourceName.c_str());
								}
							});
						}
					}

					// mark DownloadsComplete on the next frame so all resources will have started
					{
						std::unique_lock<std::recursive_mutex> lock(executeNextGameFrameMutex);
						executeNextGameFrame.push_back([=]()
						{
							netLibrary->DownloadsComplete();
						});
					}

					doneCb();
				});
			});
		};

		static std::queue<std::string> g_resourceUpdateQueue;

		std::shared_ptr<std::unique_ptr<std::function<void()>>> updateResource = std::make_shared<std::unique_ptr<std::function<void()>>>();
		*updateResource = std::make_unique<std::function<void()>>([=]()
		{
			if (!g_resourceUpdateQueue.empty())
			{
				auto resource = g_resourceUpdateQueue.front();
				g_resourceUpdateQueue.pop();

				updateResources(resource, [=]()
				{
					g_resourceStartRequestSet.erase(resource);

					std::unique_lock<std::recursive_mutex> lock(executeNextGameFrameMutex);
					executeNextGameFrame.push_back(**updateResource);
				});
			}
		});

		// download trigger
		netLibrary->OnInitReceived.Connect([=] (NetAddress address)
		{
			g_netAddress = address;

			fx::ResourceManager* resourceManager = Instance<fx::ResourceManager>::Get();
			resourceManager->ResetResources();

			updateResources("", []()
			{
			});
		});

		netLibrary->OnConnectionError.Connect([](const char* error)
		{
			std::unique_lock<std::recursive_mutex> lock(executeNextGameFrameMutex);

			executeNextGameFrame.push_back([]()
			{
				fx::ResourceManager* resourceManager = Instance<fx::ResourceManager>::Get();
				resourceManager->ResetResources();
			});
		});

		OnGameFrame.Connect([] ()
		{
			std::unique_lock<std::recursive_mutex> lock(executeNextGameFrameMutex);

			for (auto& func : executeNextGameFrame)
			{
				func();
			}

			executeNextGameFrame.clear();
		});

		netLibrary->AddReliableHandler("msgNetEvent", [] (const char* buf, size_t len)
		{
			NetBuffer buffer(buf, len);

			// get the source net ID
			uint16_t sourceNetID = buffer.Read<uint16_t>();

			// get length of event name and read the event name
			static char eventName[65536];

			uint16_t nameLength = buffer.Read<uint16_t>();
			buffer.Read(eventName, nameLength);

			// read the data
			size_t dataLen = len - nameLength - (sizeof(uint16_t) * 2);
			std::vector<char> eventData(dataLen);

			buffer.Read(&eventData[0], dataLen);

			// convert the source net ID to a string
			std::string source = "net:" + std::to_string(sourceNetID);

			// get the resource manager and eventing component
			static fx::ResourceManager* resourceManager = Instance<fx::ResourceManager>::Get();
			static fwRefContainer<fx::ResourceEventManagerComponent> eventManager = resourceManager->GetComponent<fx::ResourceEventManagerComponent>();

			// and queue the event
			eventManager->QueueEvent(std::string(eventName), std::string(&eventData[0], eventData.size()), source);
		});

		netLibrary->AddReliableHandler("msgResStop", [] (const char* buf, size_t len)
		{
			std::string resourceName(buf, len);

			fx::ResourceManager* resourceManager = Instance<fx::ResourceManager>::Get();
			resourceManager->MakeCurrent();

			auto resource = resourceManager->GetResource(resourceName);

			if (resource.GetRef() == nullptr)
			{
				trace("Server requested resource %s to be stopped, but we don't know that resource\n", resourceName.c_str());
				return;
			}

#if 0
			if (resource->GetState() != ResourceStateRunning)
			{
				trace("Server requested resource %s to be stopped, but it's not running\n", resourceName.c_str());
				return;
			}
#endif

			resource->Stop();
		});

		netLibrary->AddReliableHandler("msgResStart", [=] (const char* buf, size_t len)
		{
			std::string resourceName(buf, len);

			fx::ResourceManager* resourceManager = Instance<fx::ResourceManager>::Get();
			resourceManager->MakeCurrent();

			auto resource = resourceManager->GetResource(resourceName);

			if (resource.GetRef() != nullptr)
			{
#if 0
				if (resource->GetState() != ResourceStateStopped)
				{
					trace("Server requested resource %s to be started, but it's not stopped\n", resourceName.c_str());
					return;
				}
#endif
			}

			if (g_resourceStartRequestSet.find(resourceName) == g_resourceStartRequestSet.end())
			{
				g_resourceStartRequestSet.insert(resourceName);
				g_resourceUpdateQueue.push(resourceName);

				{
					std::unique_lock<std::recursive_mutex> lock(executeNextGameFrameMutex);
					executeNextGameFrame.push_back(**updateResource);
				}
			}
		});

		fx::ScriptEngine::RegisterNativeHandler("TRIGGER_SERVER_EVENT_INTERNAL", [=] (fx::ScriptContext& context)
		{
			std::string eventName = context.GetArgument<const char*>(0);
			size_t payloadSize = context.GetArgument<uint32_t>(2);
			
			std::string eventPayload = std::string(context.GetArgument<const char*>(1), payloadSize);

			NetBuffer buffer(131072);
			buffer.Write<uint16_t>(eventName.size() + 1);
			buffer.Write(eventName.c_str(), eventName.size() + 1);

			buffer.Write(eventPayload.c_str(), eventPayload.size());

			netLibrary->SendReliableCommand("msgServerEvent", buffer.GetBuffer(), buffer.GetCurLength());
		});


		netLibrary->OnFinalizeDisconnect.Connect([=](NetAddress)
		{
			std::unique_lock<std::recursive_mutex> lock(executeNextGameFrameMutex);

			executeNextGameFrame.push_back([]()
			{
				Instance<fx::ResourceManager>::Get()->ForAllResources([](fwRefContainer<fx::Resource> resource)
				{
					resource->GetComponent<fx::ResourceGameLifetimeEvents>()->OnGameDisconnect();
				});
			});
		});

		Instance<ICoreGameInit>::Get()->OnShutdownSession.Connect([=] ()
		{
			AddCrashometry("reset_resources", "true");

			Instance<fx::ResourceManager>::Get()->ResetResources();
		});

		console::GetDefaultContext()->GetCommandManager()->FallbackEvent.Connect([=](const std::string& cmd, const ProgramArguments& args, const std::any& context)
		{
			if (netLibrary->GetConnectionState() != NetLibrary::CS_ACTIVE)
			{
				return true;
			}

			std::string s = console::GetDefaultContext()->GetCommandManager()->GetRawCommand();

			NetBuffer buffer(131072);
			buffer.Write<uint16_t>(s.size());
			buffer.Write(s.c_str(), std::min(s.size(), static_cast<size_t>(INT16_MAX)));

			netLibrary->SendReliableCommand("msgServerCommand", buffer.GetBuffer(), buffer.GetCurLength());

			return false;
		}, 99999);
	});
});
/*


manager->AddMounter(fx::GetCachedResourceMounter(manager.GetRef(), "rescache:/"));
*/
