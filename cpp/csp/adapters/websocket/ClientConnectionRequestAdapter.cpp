#include <csp/adapters/websocket/ClientConnectionRequestAdapter.h>
#include <csp/python/Conversions.h>
#include <iostream>
#include <Python.h>

namespace csp::adapters::websocket {

ClientConnectionRequestAdapter::ClientConnectionRequestAdapter(
    Engine * engine,
    Dictionary& properties,
    int64_t caller_id,
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
    // DictionaryPtr val = input() -> lastValueTyped<DictionaryPtr>();
    std::cout << "WE ARE GETTING A DICT" << "\n";
    // Dictionary val = input() -> lastValueTyped<Dictionary>();
    auto raw_val = input() -> lastValueTyped<PyObject*>();
    Dictionary val = python::fromPython<Dictionary>( raw_val );
    std::cout << "WE GOT A DICT" << "\n";
    // Make a copy of the Dictionary, not just the pointer
    // Dictionary valueCopy = *val;  // Copy the actual dictionary
    auto valCopy = val;  // Make copy before the lambda
    boost::asio::post(m_ioc, [this, val=std::move(valCopy)]() {
        m_clientAdapterManager->handleConnectionRequest(val);
    });
    // auto& caller_id = m_callerId;
    // for( auto& update : input() -> lastValueTyped<std::vector<WebsocketHeaderUpdate::Ptr>>() )
    // { 
    //     if( update -> key_isSet() && update -> value_isSet() ) headers->update( update->key(), update->value() ); 
    // }
};

}