#include <csp/adapters/websocket/ClientHeaderUpdateAdapter.h>

namespace csp::adapters::websocket {

class WebsocketEndpointManager;

ClientHeaderUpdateOutputAdapter::ClientHeaderUpdateOutputAdapter(
    Engine * engine,
    Dictionary& properties,
    WebsocketEndpointManager * mgr
) : OutputAdapter( engine ), m_properties( properties ), m_mgr( mgr )
{ };

void ClientHeaderUpdateOutputAdapter::executeImpl()
{
    Dictionary headers;
    for (auto& update : input()->lastValueTyped<std::vector<WebsocketHeaderUpdate::Ptr>>()) {
        if (update->key_isSet() && update->value_isSet()) {
            headers.update(update->key(), update->value());
        }
    }
    auto endpoint = m_mgr -> getNonDynamicEndpoint();
    endpoint -> updateHeaders(std::move(headers));

    // Get the first e
    // ndpoint from the map and call updateHeaders
    // DictionaryPtr headers = m_properties.get<DictionaryPtr>("headers");
    // for( auto& update : input() -> lastValueTyped<std::vector<WebsocketHeaderUpdate::Ptr>>() )
    // { 
    //     if( update -> key_isSet() && update -> value_isSet() ) headers->update( update->key(), update->value() ); 
    // }
};

}