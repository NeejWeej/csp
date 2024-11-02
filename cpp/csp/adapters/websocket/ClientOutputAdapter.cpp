#include <csp/adapters/websocket/ClientOutputAdapter.h>

namespace csp::adapters::websocket {

ClientOutputAdapter::ClientOutputAdapter(
    Engine * engine,
    WebsocketEndpoint& endpoint,
    ClientAdapterManager * clientAdapterManager,
    size_t caller_id,
    net::io_context& ioc
) : OutputAdapter( engine ), 
    m_endpoint( endpoint ), 
    m_clientAdapterManager( clientAdapterManager ),
    m_callerId( caller_id ),
    m_ioc( ioc ) 
{ };

void ClientOutputAdapter::executeImpl()
{
    // TODO Add here picking the right endpoints to send to
    // Based on the caller id
    const std::string & value = input() -> lastValueTyped<std::string>();
    boost::asio::post(m_ioc, [this, value=value]() {
        // something something lifetime? Not sure
        m_clientAdapterManager->send(value, m_callerId);
    });
    // m_endpoint.send( value );
};

}