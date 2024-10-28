#ifndef _IN_CSP_ADAPTERS_WEBSOCKETS_CLIENT_ADAPTERMGR_H
#define _IN_CSP_ADAPTERS_WEBSOCKETS_CLIENT_ADAPTERMGR_H

#include <csp/adapters/websocket/WebsocketEndpoint.h>
#include <csp/adapters/websocket/ClientInputAdapter.h>
#include <csp/adapters/websocket/ClientOutputAdapter.h>
#include <csp/adapters/websocket/ClientHeaderUpdateAdapter.h>
#include <csp/adapters/websocket/ClientConnectionRequestAdapter.h>
#include <csp/core/Enum.h>
#include <csp/core/Hash.h>
#include <csp/engine/AdapterManager.h>
#include <csp/engine/Dictionary.h>
#include <csp/engine/PushInputAdapter.h>
#include <csp/core/Platform.h>
#include <thread>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <vector>
#include <unordered_set>


namespace csp::adapters::websocket {

using namespace csp;

class ClientConnectionRequestAdapter;

struct WebsocketClientStatusTypeTraits
{
    enum _enum : unsigned char
    {
        ACTIVE = 0,
        GENERIC_ERROR = 1,
        CONNECTION_FAILED = 2,
        CLOSED = 3,
        MESSAGE_SEND_FAIL = 4,

        NUM_TYPES
    };

protected:
    _enum m_value;
};

using ClientStatusType = Enum<WebsocketClientStatusTypeTraits>;

class CSP_PUBLIC ClientAdapterManager final : public AdapterManager
{
public:
    ClientAdapterManager(
        Engine * engine,
        const Dictionary & properties
    );
    ~ClientAdapterManager();

    const char * name() const override { return "WebsocketClientAdapterManager"; }

    void start( DateTime starttime, DateTime endtime ) override;

    void stop() override;

    void processConnectionRequestUpdate( const Dictionary & properties);

    PushInputAdapter * getInputAdapter( CspTypePtr & type, PushMode pushMode, const Dictionary & properties );
    OutputAdapter * getOutputAdapter();
    OutputAdapter * getHeaderUpdateAdapter();
    OutputAdapter * getConnectionRequestAdapter(const Dictionary & properties);

    DateTime processNextSimTimeSlice( DateTime time ) override;

private:
    // need some client info
    // This is a tuple of
    // (number of send calls to uri, set of caller id's subscribed to uri )
    // We use this information to keep track of how to route messages to/from
    // uri's, and when a uri connection can be closed.
    using UriInfo = std::tuple<uint64_t, std::unordered_set<uint64_t>>;
    net::io_context m_ioc;
    bool m_active;
    bool m_shouldRun;
    std::unique_ptr<WebsocketEndpoint> m_endpoint;
    ClientInputAdapter* m_inputAdapter;
    ClientOutputAdapter* m_outputAdapter;
    ClientHeaderUpdateOutputAdapter* m_updateAdapter;
    std::unique_ptr<std::thread> m_thread;
    Dictionary m_properties;
    // For each subscribe call, which uri's it is subscribed to
    std::vector<std::unordered_set<std::string>> m_subscribeFromUri;
    // For each send call, which uri's it will send out to
    std::vector<std::unordered_set<std::string>> m_sendToUri;

    // uri -> (send_calls, set of caller id's for the subscribtions)
    // If send_calls is 0 (no adapter is sending out to that uri)
    // AND the subscriptions set is empty, we can then shutdown the encpoint.
    std::unordered_map<std::string, UriInfo> m_uriInfo;
    //unclear if this is needed to be on the object
    std::vector<ClientConnectionRequestAdapter*> m_connectionRequestAdapters;
};

}

#endif
