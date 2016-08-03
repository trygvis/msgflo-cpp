#include "msgflo.h"

#include <iostream>
#include <thread>
#include <boost/asio/io_service.hpp>
#include "amqpcpp.h"
#include "mqtt_support.h"

using namespace std;
using namespace trygvis::mqtt_support;

namespace msgflo {

class AbstractMessage : public Message {
protected:
    AbstractMessage(const char *data, const uint64_t len) : _data(data), _len(len) {}

    virtual ~AbstractMessage() {};

    const char *_data;
    const uint64_t _len;

public:
    virtual void data(const char **data, uint64_t *len) override {
        *data = this->_data;
        *len = this->_len;
    }

    virtual json11::Json asJson() override {
        string err;
        return json11::Json::parse(_data, err);
    }
};

//void Participant::send(std::string port, const json11::Json &msg)
//{
//    if (!_engine) return;
//    _engine->send(port, msg);
//}
//
//void Participant::ack(Message msg) {
//    if (!_engine) return;
//    _engine->ack(msg);
//}
//
//void Participant::nack(Message msg) {
//    if (!_engine) return;
//    _engine->nack(msg);
//}

class DiscoveryMessage {
public:
    DiscoveryMessage(const Definition &def)
        : definition(def)
    {
    }

    json11::Json to_json() const {
        using namespace json11;

        return Json::object {
                {"protocol",  "discovery"},
                {"command",  "participant"},
                {"payload", definition.to_json() },
        };
    }

private:
    Definition definition;
};

class AmqpEngine final : public Engine {
    struct AmqpMessage final : public AbstractMessage {
        AmqpMessage(const char *data, uint64_t len, uint64_t deliveryTag)
            : AbstractMessage(data, len), _deliveryTag(deliveryTag) {
        }

        uint64_t _deliveryTag;

        virtual void ack() override {
        }

        virtual void nack() override {
        }
    };

public:
    AmqpEngine(Participant *p, const string &url)
        : Engine()
        , handler()
        , connection(&handler, AMQP::Address(url))
        , channel(&connection)
        , participant(p)
    {
        channel.setQos(1); // TODO: is this prefech?
        setEngine(participant, this);

        for (const auto &port : participant->definition()->inports) {
            setupInport(port);
        }
        for (const auto &port : participant->definition()->outports) {
            setupOutport(port);
        }

        sendParticipant();

        ioService.run();
    }

    bool connected() override {
        return handler.connected;
    }

private:
    void sendParticipant() {
        std::string data = json11::Json(*participant->definition()).dump();
        AMQP::Envelope env(data);
        channel.publish("", "fbp", env);
    }

    void setupOutport(const Definition::Port &p) {
        channel.declareExchange(p.queue, AMQP::fanout);
    }

    void setupInport(const Definition::Port &p) {

        channel.declareQueue(p.queue, AMQP::durable);
        channel.consume(p.queue).onReceived(
            [p, this](const AMQP::Message &message,
                       uint64_t deliveryTag,
                       bool redelivered)
            {
                static_cast<void>(redelivered);

                const auto body = message.message();
                std::cout<<" [x] Received "<<body<<std::endl;
                AmqpMessage msg(message.body(), message.bodySize(), deliveryTag);
                std::string err;
                process(this->participant, p.id, &msg);
            });

    }

public:
    void send(string port, const json11::Json &msg) override {
        const string exchange = Definition::queueForPort(participant->definition()->outports, port);
        const string data = msg.dump();
        AMQP::Envelope env(data.c_str(), data.size());
        cout << " Sending on " << exchange << " :" << data << endl;
        channel.publish(exchange, "", env);
    }

//    void ack(const Message *msg) override {
//        channel.ack(msg.deliveryTag);
//    }
//
//    void nack(const Message *msg) override {
//        static_cast<void>(msg);
//         channel.nack(msg.deliveryTag);
//    }

private:
    class MsgFloAmqpHandler : public AMQP::TcpHandler {
    public:
        bool connected;

    protected:

        void onConnected(AMQP::TcpConnection *connection) {
            static_cast<void>(connection);
            connected = true;
        }

        void onError(AMQP::TcpConnection *connection, const char *message) {
            static_cast<void>(connection);
            static_cast<void>(message);
            connected = false;
        }

        void onClosed(AMQP::TcpConnection *connection) {
            static_cast<void>(connection);
            connected = false;
        }

        void monitor(AMQP::TcpConnection *connection, int fd, int flags) {
            static_cast<void>(connection);
            static_cast<void>(fd);
            static_cast<void>(flags);
        }
    };

private:
    boost::asio::io_service ioService;
    MsgFloAmqpHandler handler;
    AMQP::TcpConnection connection;
    AMQP::TcpChannel channel;
    Participant *participant;
};

// We're all into threads
using msg_flo_mqtt_client = mqtt_client<trygvis::mqtt_support::mqtt_client_personality::threaded>;

class MosquittoEngine final : public Engine, protected mqtt_event_listener {

    struct MosquittoMessage final : public AbstractMessage {
        MosquittoMessage(const struct mosquitto_message *m)
            : AbstractMessage(static_cast<char *>(m->payload), static_cast<uint64_t>(m->payloadlen)), _mid(m->mid) {
        }

        int _mid;

        virtual void ack() override {
        }

        virtual void nack() override {
        }
    };

public:
    MosquittoEngine(const EngineConfig config, Participant *participant, const string &host, const int port,
                    const int keep_alive, const string &client_id, const bool clean_session) :
        _debugOutput(config.debugOutput()), _participant(participant), client(this, host, port, keep_alive, client_id, clean_session) {
        setEngine(participant, this);

        client.connect();
    }

    virtual ~MosquittoEngine() {
    }

    void send(string port, const json11::Json &msg) override {
        auto queue = Definition::queueForPort(_participant->definition()->outports, port);

        if (queue.empty()) {
            throw std::domain_error("No such port: " + port);
        }

        string json = msg.dump();
        client.publish(nullptr, queue, 0, false, static_cast<int>(json.size()), json.c_str());
    }

    bool connected() override {
        return client.connected();
    }

protected:
    virtual void on_msg(const string &msg) override {
        if (!_debugOutput) {
            return;
        }

        cout << "mqtt: " << msg << endl;
    }

    virtual void on_message(const struct mosquitto_message *message) override {
        string topic = message->topic;
        for (auto &p : _participant->definition()->inports) {
            if (p.queue == topic) {
                MosquittoMessage m(message);

                // p.id should p.role
                process(_participant, p.id, &m);
            }
        }
    }

    virtual void on_connect(int rc) override {
        auto d = _participant->definition();
        string data = json11::Json(DiscoveryMessage(*d)).dump();
        client.publish(nullptr, "fbp", 0, false, data);

        for (auto &p : d->inports) {
            on_msg("Connecting port " + p.id + " to mqtt topic " + p.queue);
            client.subscribe(nullptr, p.queue, 0);
        }
    }

private:
    const bool _debugOutput;
    Participant *_participant;
    msg_flo_mqtt_client client;
};

shared_ptr<Engine> createEngine(const EngineConfig config) {

    string url = config.url();

    if (url.empty()) {
        const char* broker = std::getenv("MSGFLO_BROKER");
        if (broker) {
            url = std::string(broker);
        }
    }

    Participant *participant = config.participant();

    if (participant == nullptr) {
        throw domain_error("Bad config: participant is not set.");
    }

    if (boost::starts_with(url, "mqtt://")) {
        string host, username, password;
        int port = 1883;
        int keep_alive = 180;
        string client_id;
        bool clean_session = true;

        string s = url.substr(7);
        auto i_up = s.find('@');

        if (i_up != string::npos) {
            string up = s.substr(0, i_up);
            cout << "up: " << up << endl;

            auto i_u = up.find(':');

            if (i_u != string::npos) {
                username = up.substr(0, i_u);
                password = up.substr(i_u + 1);
            } else {
                username = up;
            }
            cout << "username: " << username << endl;
            cout << "password: " << password << endl;

            s = s.substr(i_up + 1);
            cout << "s: " << s << endl;
        }

        auto i_q = s.find('?');

        if (i_q != string::npos) {
            host = s.substr(0, i_q);
            s = s.substr(i_q + 1);
            cout << "s: " << s << endl;

            while (!s.empty()) {
                auto i_amp = s.find('&');

                string kv;
                if (i_amp == string::npos) {
                    kv = s;
                    s = "";
                } else {
                    kv = s.substr(0, i_amp);
                }
                cout << "kv: " << kv << endl;

                auto i_eq = kv.find('=');

                string key, value;
                if (i_eq != string::npos) {
                    key = kv.substr(0, i_eq);
                    value = kv.substr(i_eq + 1);
                } else {
                    key = kv;
                }

                if (key == "keepAlive") {
                    try {
                        auto v = stoul(value);
                        if (v > INT_MAX) {
                            throw invalid_argument("too big");
                        }
                        keep_alive = static_cast<int>(v);
                    } catch (invalid_argument &e) {
                        throw invalid_argument("Bad keepAlive argument, must be a number greater than zero.");
                    } catch (out_of_range &e) {
                        throw invalid_argument("Bad keepAlive argument, must be a number greater than zero.");
                    }
                } else if (key == "clientId") {
                    client_id = value;
                } else if (key == "cleanSession") {
                    clean_session = !(value == "0" || value == "no" || value == "false");
                } else {
                    // ignore unknown keys
                }

                if (i_amp == string::npos) {
                    break;
                }
                s = s.substr(i_amp + 1);
                cout << "s: " << s << endl;
            }
        } else {
            host = s;
        }

        if (config.debugOutput()) {
            cout << "host: " << host << endl;
            cout << "client_id: " << client_id << endl;
            cout << "keep_alive: " << keep_alive << endl;
            cout << "clean_session: " << clean_session << endl;
        }

        return make_shared<MosquittoEngine>(config, participant, host, port, keep_alive, client_id, clean_session);
    } else if (boost::starts_with(url, "amqp://")) {
        return make_shared<AmqpEngine>(participant, url);
    }

    throw std::runtime_error("Unsupported URL scheme: " + url);
}

} // namespace msgflo
