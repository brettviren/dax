// This ugly, self contained test exercises dynamic discovery and
// connection using Zyre.
//
// It creates an "adder" actor which hard-wires creation of PUB
// (output) and SUB (input) sockets initially neither bound nor
// connected.  A second actor combines an "adder" with a "zyre" actor.
// It tells its "adder" to bind, announces to Zyre the bound address
// and listens to zyre for any wanted peers.  When found it tells its
// "adder" to connect to the announced peer address.  Periodically it
// queries its "adder" for its current value.  Peer adders will send
// SUB input to PUB output with a value one larger.  The main()
// combines two zyre+adders in a cycle.

#include <czmq.h>
#include <zyre.h>

#include "json.hpp"
using json = nlohmann::json;

char* get_hostname()
{
    zactor_t *beacon = zactor_new (zbeacon, NULL);
    assert (beacon);
    zsock_send (beacon, "si", "CONFIGURE", 31415);
    char *hostname = zstr_recv (beacon);
    zactor_destroy (&beacon);
    return hostname;
}

static void
adder_actor(zsock_t* pipe, void* vargs)
{
    // 0. convert to using zloop and handler functions
    // 1. auto cfg = json::parse((char*)vargs);
    // 2. loop over cfg["sockets"]
    // 3. each gives ztype, a name, a role (eg "input" vs "output")
    // 4. create and associate with zlooper handler function based based on role
    // 5. store name->zsock_t* map
    // 6. extend CONNECT/BIND command messages provide name

    char* hostname = get_hostname();

    zsock_t* isock = zsock_new(ZMQ_SUB);
    assert(isock);
    zsock_set_subscribe(isock, "");

    zsock_t* osock = zsock_new(ZMQ_PUB);
    assert(osock);
    
    zpoller_t* poller = zpoller_new(pipe, isock, NULL);
    assert(poller);
    
    zsys_info("adder starting");
    zsock_signal(pipe, 0);

    int number = 0;

    while (!zsys_interrupted) {
        void* which = zpoller_wait(poller, -1);

        if (which == pipe) {
            zmsg_t *msg = zmsg_recv (pipe);
            if (!msg) {
                break;
            }
            char *method = zmsg_popstr (msg);
    
            if (streq (method, "BIND")) {
                char *endpoint = zmsg_popstr (msg);
                int port = zsock_bind(osock, "%s", endpoint);
                assert (port > 0);
                free(endpoint);
                endpoint = zsys_sprintf("tcp://%s:%d", hostname, port);
                zstr_sendm (pipe, "BIND");
                zstr_sendf (pipe, "%s", endpoint);
                zsys_info("adder bind to %s", endpoint);
                free(endpoint);
            }
            else if (streq (method, "CONNECT")) {
                char *endpoint = zmsg_popstr (msg);
                zsock_connect(isock, endpoint);
                zstr_sendm (pipe, "CONNECT");
                zstr_sendf (pipe, "%s", endpoint);
                zsys_info("adder connect to %s", endpoint);
                free (endpoint);
                if (number == 0) {
                    zclock_sleep(100);
                    zsock_send(osock, "i", number);
                }
            }
            else if (streq (method, "VALUE")) {
                zsys_info("adder sending requested VALUE: %d", number);
                zsock_send(pipe, "si", "VALUE", number);
            }
            free (method);
            zmsg_destroy(&msg);
            continue;
        }
        
        if (which == isock) {
            zsock_recv(isock, "i", &number);
            if (number > 100000) {
                zsys_info("adder: it's big enough already");
                zsock_signal(pipe, 1);
                break;
            }
            ++number;           // the actual payload operation
            zsock_send(osock, "i", number);
            //zsys_debug("number: %d", number);
            continue;
        }

        if (zpoller_terminated(poller)) {
            break;
        }
    }
            
  cleanup:
    zpoller_destroy(&poller);
    zsock_destroy(&osock);
    zsock_destroy(&isock);
}

// This combines zyre and adder actors to let zyre announcements drive
// connections.  
static void
zyre_adder_actor(zsock_t* pipe, void* vargs)
{
    auto cfg = json::parse((char*)vargs);
    std::string name = cfg["name"];
    std::string want = cfg["want"];

    zsock_signal(pipe, 0);

    zactor_t* adder = zactor_new(adder_actor, NULL);
    zsock_send(zactor_sock(adder), "ss", "BIND", "tcp://*:*");
    char* bind_address = NULL;
    zsock_recv(zactor_sock(adder), "ss", NULL, &bind_address);
    zsys_info("zyre (\"%s\") bind to %s", name.c_str(), bind_address);

    zyre_t* zyre = zyre_new(name.c_str());
    zyre_set_header(zyre, name.c_str(), "%s", bind_address);
    free(bind_address);
    zyre_start(zyre);

    zpoller_t* poller = zpoller_new(pipe, zactor_sock(adder), zyre_socket(zyre), NULL);

    while (!zsys_interrupted) {

        void* which = zpoller_wait(poller, 1000);
        
        if (which == pipe) {
            break;
        }

        if (!which) {           // timeout
            //zsys_info("zyre (\"%s\") sending VALUE", name.c_str());
            zsock_send(zactor_sock(adder), "s", "VALUE");
            continue;
        }

        if (which == zyre_socket(zyre)) {
            zyre_event_t* zev = zyre_event_new(zyre);
            const char* zevent = zyre_event_type(zev);
            zsys_info("zyre (\"%s\") event \"%s\"", name.c_str(), zevent);
            if (streq(zevent, "ENTER")) {
                const char* endpoint = zyre_event_header(zev, want.c_str());
                if (endpoint) {
                    zsys_info("zyre (\"%s\") sending CONNECT %s", name.c_str(), endpoint);
                    zsock_send(zactor_sock(adder), "ss", "CONNECT", endpoint);
                }
            }
            zyre_event_destroy(&zev);
            continue;
        }
        if (which == zactor_sock(adder)) {
            zmsg_t *msg = zmsg_recv(zactor_sock(adder));
            char* method = zmsg_popstr(msg);
            if (streq(method, "CONNECT")) {
                char* endpoint = zmsg_popstr(msg);
                zsys_info("Connected to %s", endpoint);
                free (endpoint);
            }
            else if (streq(method, "VALUE")) {
                zsys_info("zyre (\"%s\") receiving VALUE", name.c_str());
                zframe_t* fr = zmsg_pop(msg);
                const int value = *(int*)zframe_data(fr); // sketchy as hell
                zframe_destroy(&fr);
                zsys_info("zyre (\"%s\") value is %d", name.c_str(), value);
            }
            else {
                break;
            }
            free(method);
            zmsg_destroy(&msg);
            continue;
        }

        if (zpoller_terminated (poller)) {
            break;
        }
    }

    zpoller_destroy(&poller);
    zyre_destroy(&zyre);
    zactor_destroy(&adder);
    
}

int main()
{
    zsys_init();
    const char* cfg1 = R"({"name":"adder1","want":"adder2"})";
    zactor_t* za1 = zactor_new(zyre_adder_actor, (void*)cfg1);
    const char* cfg2 = R"({"name":"adder2","want":"adder1"})";
    zactor_t* za2 = zactor_new(zyre_adder_actor, (void*)cfg2);

    zpoller_t* poller = zpoller_new(zactor_sock(za1), zactor_sock(za2), NULL);

    while (!zsys_interrupted) {
        void* which = zpoller_wait(poller, -1);
        if (which) {
            break;
        }
        break;
    }

    zpoller_destroy(&poller);
    zactor_destroy(&za2);
    zactor_destroy(&za1);
    return 0;
}
