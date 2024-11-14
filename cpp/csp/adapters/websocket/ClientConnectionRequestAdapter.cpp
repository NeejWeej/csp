#include <csp/adapters/websocket/ClientConnectionRequestAdapter.h>
#include <csp/python/Conversions.h>
#include <Python.h>

namespace csp::adapters::websocket {

ClientConnectionRequestAdapter::ClientConnectionRequestAdapter(
    Engine * engine,
    ClientAdapterManager * clientAdapterManager,
    net::io_context& ioc,
    bool is_subscribe,
    size_t caller_id
) : OutputAdapter( engine ),  
    m_clientAdapterManager( clientAdapterManager ),
    m_ioc( ioc),
    m_isSubscribe( is_subscribe ),
    m_callerId( caller_id )
{ };

void ClientConnectionRequestAdapter::executeImpl()
{
    if (m_isSubscribe && m_clientAdapterManager->adapterPruned(m_callerId)){
        // If the corresponding adapter is an input adapter, there is a chance
        // it was pruned from the graph. In that case, this output adapter
        // would do nothing.
        return;
    }
    auto raw_val = input() -> lastValueTyped<PyObject*>();
    Dictionary val = python::fromPython<Dictionary>( raw_val );
    // Dictionary val = Dictionary( python::fromPython<Dictionary>( raw_val )) ;
    // std::cout << "WE GOT A DICT" << "\n";
    // Make a copy of the Dictionary, not just the pointer
    // Dictionary valueCopy = *val;  // Copy the actual dictionary
    boost::asio::post(m_ioc, [this, val=std::move(val)]() {
        m_clientAdapterManager->handleConnectionRequest(val);
    });
};

}