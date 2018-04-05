#include <iostream>
#include <sstream>
#include <mutex>
#include <thread>
#include <chrono>
#include <atomic>
#include <string>
#include <unordered_map>

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/algorithm/string.hpp>

using tcp = boost::asio::ip::tcp;               // from <boost/asio/ip/tcp.hpp>
namespace websocket = boost::beast::websocket;  // from <boost/beast/websocket.hpp>

using namespace std::chrono;

#include <Aw.h>

typedef void* AWInstance;


#include "botsettings.hpp"

using namespace std;

void event_avatar_add();
void event_avatar_delete();
void event_chat();

static void write_message(std::string message);

struct websocket_client;

websocket_client * client = nullptr;
bool is_steady = false;
system_clock::time_point started;

std::mutex liu_mtx;
std::unordered_map<int, std::string> logged_in_users;


struct websocket_client {
    boost::asio::io_context ioc;
    tcp::resolver resolver;
    websocket::stream<tcp::socket> ws;
    std::mutex read_mtx;
    std::mutex reconnect_mtx;
    vpdiscordbot::Settings * settings;
    std::atomic_bool isAlive;

    std::vector<vpdiscordbot::Message> messages_to_send;

    bool shouldContinue = true;
    bool connected = true;

    websocket_client() : ioc{}, resolver{ ioc }, ws{ ioc }, messages_to_send{}, isAlive{ true } {
        connect();
    }

    void close() {
        if (connected) {
            try {
                ws.close(websocket::close_code::normal);
            } catch (std::exception&) {}

            ws = websocket::stream<tcp::socket> { ioc };

            connected = false;
        }
    }

    void reconnect() {
        if (reconnect_mtx.try_lock()) {
            close();
            cout << "Error. Lost connection. Reconnecting..." << endl;
            connect();
            reconnect_mtx.unlock();
        }
    }

    void connect() {
        auto const results = resolver.resolve("localhost", "7414");
        bool connected = false;
        bool upgraded = false;

        while (!connected) {
            try {
                boost::asio::connect(ws.next_layer(), results.begin(), results.end());
                connected = true;
            } catch (std::exception&) {}
        }

        while (!upgraded) {
            ws.handshake("localhost", "/");
            boost::beast::multi_buffer buffer;
            ws.read(buffer);
            std::stringstream ss;
            ss << boost::beast::buffers(buffer.data());
            std::string data = ss.str();
            if (data == std::string("connected")) {
                upgraded = true;
            }
        }
    }

    ~websocket_client() {
        if (connected) {
            close();
        }
    }
};

static void websocket_service() {
    while (client->shouldContinue) {
        // Parse websocket messages
        boost::beast::multi_buffer buffer;

        try {
            client->ws.read(buffer);
        } catch (std::exception&) {
            client->reconnect();
            continue;
        }

        cout << "From Discord: " << boost::beast::buffers(buffer.data()) << endl;
        std::stringstream ss;
        ss << boost::beast::buffers(buffer.data());
        std::string data = ss.str();
        vpdiscordbot::Message message = vpdiscordbot::GetMessageFromJson(data);
        if (message.Malformed) {
            cout << "Malformed message. Skipping..." << endl;
        } else {
            if (message.message.substr(0, 7) == std::string("!online")) {
                // List the online users.
                ss.str("");
                std::stringstream ss2, ss3;
                ss << "**Online users currently in " << client->settings->bot.world << "**\\n";
                std::lock_guard<std::mutex> lock{liu_mtx};
                int count_norm = 0;
                int count_bots = 0;
                int count_irc = 0;
                for ( auto p : logged_in_users ) {
                    if (p.second.substr(0, 4) == std::string("[irc")) {
                        // Is an IRC bot
                        count_irc++;
                        ss3 << p.second << ", ";
                    } else if (p.second.substr(0, 1) == std::string("[")) {
                        // Is a bot
                        count_norm++;
                        ss2 << p.second << ", ";
                    } else {
                        // Is a user
                        count_bots++;
                        ss << p.second << ", ";
                    }
                }
                std::string online_msg = ss.str();
                std::string bot_online_msg = ss2.str();
                std::string irc_online_msg = ss3.str();
                if (count_norm > 0)
                    online_msg = online_msg.substr(0, online_msg.length() - 2);

                if (count_bots > 0)
                    bot_online_msg = bot_online_msg.substr(0, bot_online_msg.length() - 2);

                if (count_irc > 0)
                    irc_online_msg = irc_online_msg.substr(0, irc_online_msg.length() - 2);

                ss.str("");
                ss << "{ \"name\" : \"The Bridge\", \"message\": \"" << online_msg << "\\n\\n**Bots Online**\\n" << bot_online_msg << "\\n\\n**IRC Bots Online**\\n" << irc_online_msg << "\" }";
                cout << ss.str() << endl;
                write_message(ss.str());
            }

            std::lock_guard<std::mutex> lock{client->read_mtx};
            client->messages_to_send.push_back(message);
        }
    }
}

static void write_message(std::string msg) {
    try {
        client->ws.write(boost::asio::buffer(msg));
    } catch (std::exception&) {
        client->reconnect();
    }
}

int main(int argc, char ** argv) {
    vpdiscordbot::Settings settings;
    websocket_client websocket;

    client = &websocket;

    thread ws_service(websocket_service);

    try {
        settings = vpdiscordbot::GetSettingsFromFile("../../Configuration/bot-configuration.json");
    } catch (exception& err) {
        cerr << "ERROR: " << err.what() << endl;
        return -1;
    }

    websocket.settings = &settings;

    int err = 0;

    AWInstance sdk;

    if (err = aw_init(AW_BUILD)) {
        cerr << "ERROR: Couldn't initialize VP API (reason " << err << ")" << endl;
        return -err;
    }

    if (err = aw_create(NULL, 0, &sdk)) {
        cerr << "ERROR: Couldn't connect to universe (reason " << err << ")" << endl;
        return -err;
    }

	aw_int_set(AW_LOGIN_OWNER, settings.auth.username);
	aw_string_set(AW_LOGIN_PRIVILEGE_PASSWORD, settings.auth.password.c_str());
	aw_string_set(AW_LOGIN_NAME, settings.bot.name.c_str());


    if (err = aw_login()) {
        cerr << "ERROR: Couldn't login (reason " << err << ")" << endl;
        return -err;
    }

    //if (err = vp_world_setting_set(sdk))

    if (err = aw_enter(settings.bot.world.c_str())) {
        cerr << "ERROR: Couldn't enter " << settings.bot.world << " (reason " << err << ")" << endl;
        return -err;
    } else {
        cout << "Entered world..." << endl;
    }

    started = system_clock::now();
    aw_event_set(AW_EVENT_AVATAR_ADD, event_avatar_add);
    aw_event_set(AW_EVENT_AVATAR_DELETE, event_avatar_delete);
    aw_event_set(AW_EVENT_CHAT, event_chat);
    aw_state_change();

    while (aw_wait(100) == 0) {
        std::this_thread::sleep_for(milliseconds(50));
        std::lock_guard<std::mutex> lock{ websocket.read_mtx };
		aw_bool_set(AW_CONSOLE_ITALICS, 0);
		aw_bool_set(AW_CONSOLE_BOLD, 0);
		aw_int_set(AW_CONSOLE_RED, 0);
		aw_int_set(AW_CONSOLE_GREEN, 0);
		aw_int_set(AW_CONSOLE_BLUE, 0);
        for (auto message : websocket.messages_to_send) {
			std::string full_message;
			full_message.append(message.name);
			full_message.append(":\t");
			full_message.append(message.message);
			aw_string_set(AW_CONSOLE_MESSAGE, full_message.c_str());
			aw_console_message(0);
        }
        websocket.messages_to_send.clear();
    }
    websocket.shouldContinue = false;
    websocket.close();
    ws_service.join();

    return 0;
}

void event_avatar_add() {
    int session(aw_int(AW_AVATAR_SESSION));
    std::string name(aw_string(AW_AVATAR_NAME));


    std::lock_guard<std::mutex> lock{liu_mtx};
    logged_in_users[session] = name;


    if (!is_steady && duration_cast<seconds>(system_clock::now() - started).count() > 5) {
        cout << "Now Steady." << endl;
        is_steady = true;
    } else if (!is_steady) {
        cout << "User Joined: " << name << endl;
        return;
    }

    stringstream ss;
    ss << "{ \"name\" : \"vp-" << name << "\", \"message\": \"**Has joined " << client->settings->bot.world << ".**\" }";
    cout << ss.str() << endl;
    write_message(ss.str());
}

void event_avatar_delete() {
    int session(aw_int(AW_AVATAR_SESSION));
    std::string name(aw_string(AW_AVATAR_NAME));
    stringstream ss;
    ss << "{ \"name\" : \"vp-" << name << "\", \"message\": \"**Has left " << client->settings->bot.world << ".**\" }";
    cout << ss.str() << endl;
    write_message(ss.str());
    std::lock_guard<std::mutex> lock{liu_mtx};
    logged_in_users.erase(session);
}

void event_chat() {
    stringstream ss;
    std::string name(aw_string(AW_AVATAR_NAME));
    std::string message(aw_string(AW_CHAT_MESSAGE));

    if (name.substr(0, 3) == std::string("[d-")) {
        return; // Ignore discord messages.
    }

    // Let's escape the message.
    boost::replace_all(message, "\"", "\\\"");

    ss << "{ \"name\" : \"vp-" << name << "\", \"message\": \"" << message << "\" }";
    cout << ss.str() << endl;
    write_message(ss.str());
}
