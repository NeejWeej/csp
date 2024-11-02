#ifndef _IN_CSP_ADAPTERS_WEBSOCKETS_CLIENT_OUTPUTADAPTER_H
#define _IN_CSP_ADAPTERS_WEBSOCKETS_CLIENT_OUTPUTADAPTER_H

#include <csp/adapters/websocket/WebsocketEndpoint.h>
#include <csp/engine/Dictionary.h>
#include <csp/engine/OutputAdapter.h>
#include <csp/adapters/utils/MessageWriter.h>
#include <csp/adapters/websocket/ClientAdapterManager.h> 

namespace csp::adapters::websocket
{

class ClientAdapterManager;

class ClientOutputAdapter final: public OutputAdapter
{

public:
    ClientOutputAdapter(
        Engine * engine,
        WebsocketEndpoint& endpoint,
        ClientAdapterManager * clientAdapterManager,
        int64_t caller_id,
        net::io_context& ioc
    );

    void executeImpl() override;

    const char * name() const override { return "WebsocketClientOutputAdapter"; }

private:
    [[maybe_unused]] WebsocketEndpoint& m_endpoint;
    ClientAdapterManager* m_clientAdapterManager;
    int32_t m_callerId;
    net::io_context& m_ioc;
    // std::unordered_map<std::string, std::vector<bool>>& m_endpoint_consumers;
};

}


#endif