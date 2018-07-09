#pragma once

namespace fx
{
	namespace ServerDecorators
	{
		template<uint32_t I>
		inline uint32_t const_uint32()
		{
			return I;
		}

		template<typename... THandler>
		const fwRefContainer<fx::GameServer>& WithPacketHandler(const fwRefContainer<fx::GameServer>& server)
		{
			server->SetComponent(new HandlerMapComponent());

			// store the handler map
			HandlerMapComponent* map = server->GetComponent<HandlerMapComponent>().GetRef();

			server->SetPacketHandler([=](uint32_t packetId, const std::shared_ptr<fx::Client>& client, net::Buffer& packet)
			{
				bool handled = false;

				// any fast-path handlers?
				pass{ ([&]
				{
					if (!handled && packetId == const_uint32<HashRageString(THandler::GetPacketId())>())
					{
						THandler::Handle(server->GetInstance(), client, packet);
						handled = true;
					}
				}(), 1)... };

				// regular handler map
				if (!handled)
				{
					gscomms_execute_callback_on_main_thread([=]() mutable
					{
						auto scope = client->EnterPrincipalScope();

						auto entry = map->Get(packetId);

						if (entry)
						{
							(*entry)(client, packet);
						}
					});
				}
			});

			return server;
		}
	}
}
