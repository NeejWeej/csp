#include <csp/adapters/websocket/ClientConnectionRequestAdapter.h>

namespace csp::adapters::websocket {

ClientConnectionRequestAdapter::ClientConnectionRequestAdapter(
    Engine * engine,
    Dictionary& properties,
    uint64_t caller_id,
    ClientAdapterManager * clientAdapterManager,
    net::io_context& ioc
) : OutputAdapter( engine ), 
    m_properties( properties ), 
    m_callerId( caller_id ), 
    m_clientAdapterManager( clientAdapterManager ),
    m_ioc( ioc) 
{ };

void ClientConnectionRequestAdapter::executeImpl()
{
    DictionaryPtr val = input() -> lastValueTyped<DictionaryPtr>();

    // Make a copy of the Dictionary, not just the pointer
    Dictionary valueCopy = *val;  // Copy the actual dictionary

    boost::asio::post(m_ioc, [this, valueCopy]() {
        m_clientAdapterManager->processConnectionRequestUpdate(valueCopy);
    });
    // auto& caller_id = m_callerId;
    // for( auto& update : input() -> lastValueTyped<std::vector<WebsocketHeaderUpdate::Ptr>>() )
    // { 
    //     if( update -> key_isSet() && update -> value_isSet() ) headers->update( update->key(), update->value() ); 
    // }
};

}