#include "lws.h"

#include <node/node.h>
#include <node/node_buffer.h>
#include <cstring>
#include <iostream>
#include <future>

using namespace std;
using namespace v8;

void Main(Local<Object> exports);
void constructor(const FunctionCallbackInfo<Value> &args);
void on(const FunctionCallbackInfo<Value> &args);
void send(const FunctionCallbackInfo<Value> &args);
void setUserData(const FunctionCallbackInfo<Value> &args);
void getUserData(const FunctionCallbackInfo<Value> &args);

NODE_MODULE(lws, Main)

// these could be stored in the Server object as 3 aligned pointers?
Persistent<Function> connectionCallback, closeCallback, messageCallback;
Persistent<Object> persistentSocket;

void Main(Local<Object> exports)
{
    Isolate *isolate = exports->GetIsolate();
    Local<FunctionTemplate> tpl = FunctionTemplate::New(isolate, constructor);
    tpl->InstanceTemplate()->SetInternalFieldCount(1);

    NODE_SET_PROTOTYPE_METHOD(tpl, "on", on);
    NODE_SET_PROTOTYPE_METHOD(tpl, "send", send);
    NODE_SET_PROTOTYPE_METHOD(tpl, "setUserData", setUserData);
    NODE_SET_PROTOTYPE_METHOD(tpl, "getUserData", getUserData);

    exports->Set(String::NewFromUtf8(isolate, "Server"), tpl->GetFunction());

    Local<ObjectTemplate> socketTemplate = ObjectTemplate::New(isolate);
    socketTemplate->SetInternalFieldCount(2);
    persistentSocket.Reset(isolate, socketTemplate->NewInstance());
}

void constructor(const FunctionCallbackInfo<Value> &args)
{
    if (args.IsConstructCall()) {
        Local<Object> options = args[0]->ToObject();
        int port = options->Get(String::NewFromUtf8(args.GetIsolate(), "port"))->ToInteger()->Value();
        String::Utf8Value path(options->Get(String::NewFromUtf8(args.GetIsolate(), "path")));
        int keepAliveTime = options->Get(String::NewFromUtf8(args.GetIsolate(), "keepAliveTime"))->ToInteger()->Value();
        int keepAliveInterval = options->Get(String::NewFromUtf8(args.GetIsolate(), "keepAliveInterval"))->ToInteger()->Value();
        int keepAliveRetry = options->Get(String::NewFromUtf8(args.GetIsolate(), "keepAliveRetry"))->ToInteger()->Value();

#ifdef VERBOSE_SERVER
        cout << "Using port = " << port << ", path = " << string(*path, path.length())
             << ", keepAliveTime = " << keepAliveTime << ", keepAliveInterval = "
             << keepAliveInterval << ", keepAliveRetry = " << keepAliveRetry << endl;
#endif

        args.This()->SetAlignedPointerInInternalField(0, new lws::Server(port, string(*path, path.length()).c_str(), keepAliveTime, keepAliveRetry, keepAliveInterval));
        args.GetReturnValue().Set(args.This());
    }
}

inline Local<Object> wrapSocket(lws::Socket *socket, Isolate *isolate)
{
    struct SocketWrapper : lws::Socket {
        Local<Object> wrap(Local<Object> s)
        {
            s->SetAlignedPointerInInternalField(0, wsi);
            s->SetAlignedPointerInInternalField(1, extension);
            return s;
        }
    };

    return ((SocketWrapper *) socket)->wrap(Local<Object>::New(isolate, persistentSocket));
}

inline lws::Socket unwrapSocket(Local<Object> object)
{
    return lws::Socket((lws::clws::lws *) object->GetAlignedPointerFromInternalField(0),
                                          object->GetAlignedPointerFromInternalField(1));
}

void on(const FunctionCallbackInfo<Value> &args)
{
    lws::Server *server = (lws::Server *) args.Holder()->GetAlignedPointerFromInternalField(0);
    Isolate *isolate = args.GetIsolate();

    String::Utf8Value eventName(args[0]->ToString());
    if (!strncmp(*eventName, "connection", eventName.length())) {
        connectionCallback.Reset(isolate, Local<Function>::Cast(args[1]));
        server->onConnection([isolate](lws::Socket socket) {
            *socket.getUser() = nullptr;
            HandleScope hs(isolate);
            Local<Value> argv[] = {wrapSocket(&socket, isolate)->Clone()};
            Local<Function>::New(isolate, connectionCallback)->Call(Null(isolate), 1, argv);
        });
    }
    else if (!strncmp(*eventName, "close", eventName.length())) {
        closeCallback.Reset(isolate, Local<Function>::Cast(args[1]));
        server->onDisconnection([isolate](lws::Socket socket) {
            HandleScope hs(isolate);
            Local<Value> argv[] = {wrapSocket(&socket, isolate)};
            Local<Function>::New(isolate, closeCallback)->Call(Null(isolate), 1, argv);
            delete ((string *) *socket.getUser());
        });
    }
    else if (!strncmp(*eventName, "message", eventName.length())) {
        messageCallback.Reset(isolate, Local<Function>::Cast(args[1]));
        server->onMessage([isolate](lws::Socket socket, char *data, size_t length, bool binary) {
            HandleScope hs(isolate);
            Local<Value> argv[] = {wrapSocket(&socket, isolate),
                                   /*ArrayBuffer::New(isolate, data, length)*/
                                   node::Buffer::New(isolate, data, length, [](char *data, void *hint) {}, nullptr).ToLocalChecked(),
                                   Boolean::New(isolate, binary)};
            Local<Function>::New(isolate, messageCallback)->Call(Null(isolate), 3, argv);
        });
    }
}

// stores strings for now
void setUserData(const FunctionCallbackInfo<Value> &args)
{
    lws::Socket socket = unwrapSocket(args[0]->ToObject());
    *socket.getUser() = new string(*String::Utf8Value(args[1]->ToString()));
}

void getUserData(const FunctionCallbackInfo<Value> &args)
{
    lws::Socket socket = unwrapSocket(args[0]->ToObject());
    args.GetReturnValue().Set(String::NewFromUtf8(args.GetIsolate(), ((string *) *socket.getUser())->c_str()));
}

//Server.send(Buffer, Boolean)
void send(const FunctionCallbackInfo<Value> &args)
{
    unwrapSocket(args[0]->ToObject())
            .send(node::Buffer::Data(args[1])
            , node::Buffer::Length(args[1])
            , args[2]->BooleanValue());
}

// todo: zero-copy from JavaScript
/*void sendPaddedBuffer(const FunctionCallbackInfo<Value> &args)
{
    unwrapSocket(args[0]->ToObject())
            .send(node::Buffer::Data(args[1])
            ,node::Buffer::Length(args[1])
            , false);
}*/
