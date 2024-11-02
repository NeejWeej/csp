#ifndef _IN_CSP_ADAPTERS_WEBSOCKETS_CLIENT_CONNECTIONREQUESTADAPTER_H
#define _IN_CSP_ADAPTERS_WEBSOCKETS_CLIENT_CONNECTIONREQUESTADAPTER_H

#include <csp/engine/Dictionary.h>
#include <csp/engine/OutputAdapter.h>
#include <csp/adapters/utils/MessageWriter.h>
#include <csp/adapters/websocket/csp_autogen/websocket_types.h>
#include <csp/adapters/websocket/ClientAdapterManager.h>  // CAN I DO THIS??

namespace csp::adapters::websocket
{
using namespace csp::autogen;

class ClientAdapterManager;

class ClientConnectionRequestAdapter final: public OutputAdapter
{
public:
    ClientConnectionRequestAdapter(
        Engine * engine,
        Dictionary& properties,
        int64_t caller_id,
        ClientAdapterManager * clientAdapterManager,
        net::io_context& ioc
    );

    void executeImpl() override;

    const char * name() const override { return "WebsocketClientConnectionRequestAdapter"; }

private:
    [[maybe_unused]] Dictionary& m_properties;
    [[maybe_unused]] uint64_t m_callerId;  // caller id for associated ticking edge
    [[maybe_unused]] ClientAdapterManager* m_clientAdapterManager;
    [[maybe_unused]] net::io_context& m_ioc;

};

}


#endif