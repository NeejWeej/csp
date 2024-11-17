#include <csp/adapters/websocket/ClientConnectionRequestAdapter.h>
#include <csp/python/Conversions.h>
#include <Python.h>

namespace csp::adapters::websocket {

ClientConnectionRequestAdapter::ClientConnectionRequestAdapter(
    Engine * engine,
    WebsocketEndpointManager * websocketManager,
    net::io_context& ioc,
    bool is_subscribe,
    size_t caller_id
) : OutputAdapter( engine ),  
    m_websocketManager( websocketManager ),
    m_ioc( ioc),
    m_isSubscribe( is_subscribe ),
    m_callerId( caller_id )
{ };

void ClientConnectionRequestAdapter::executeImpl()
{
    if (m_isSubscribe && m_websocketManager ->adapterPruned(m_callerId)){
        // If the corresponding adapter is an input adapter, there is a chance
        // it was pruned from the graph. In that case, this output adapter
        // would do nothing.
        return;
    }
    auto raw_val = input() -> lastValueTyped<PyObject*>();
    Dictionary val = python::fromPython<Dictionary>( raw_val );
    // We intentionally post here, we want the thread running
    // m_ioc to handle the connection request. We want to keep
    // all updates to internal data structures at graph run-time
    // to that thread.
    boost::asio::post(m_ioc, [this, val=std::move(val)]() {
        m_websocketManager -> handleConnectionRequest(val);
    });
};

}