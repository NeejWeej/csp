#include <csp/adapters/websocket/ClientOutputAdapter.h>

namespace csp::adapters::websocket {

ClientOutputAdapter::ClientOutputAdapter(
    Engine * engine,
    WebsocketEndpointManager * websocketManager,
    size_t caller_id,
    net::io_context& ioc
) : OutputAdapter( engine ), 
    m_websocketManager( websocketManager ),
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
        m_websocketManager->send(value, m_callerId);
    });
    // if( m_dynamic ){
    //     boost::asio::post(m_ioc, [this, value=value]() {
    //         // something something lifetime? Not sure
    //         m_websocketManager->send(value, m_callerId);
    //     });
    // }
    // else{
    //     m_endpoint.send( value );
    // }
};

}