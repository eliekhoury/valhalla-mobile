
#include <valhalla/worker.h>
#include "main.h"
#include "valhalla_actor.h"

#ifdef __ANDROID__
// The Android JNI interface uses a different function signature.
#include <jni.h>

extern "C"
JNIEXPORT jstring

JNICALL
Java_com_valhalla_valhalla_ValhallaKotlin_route(JNIEnv *env,
                                                jobject thiz,
                                                jstring jRequest,
                                                jstring jConfigPath) {
    
    const char *request = env->GetStringUTFChars(jRequest, 0);
    const char *config_path = env->GetStringUTFChars(jConfigPath, 0);

    std::string result;
    try {
        // TODO: Android currently creates a new actor every time. Optimize to be like iOS later.
        ValhallaActor valhallaActor(config_path);
        result = valhallaActor.route(request);
    } catch (const valhalla::valhalla_exception_t &err) {
        printf("[ValhallaActor] route valhalla_exception: %s\n", err.what());
        std::string code = std::to_string(err.code);
        std::string message = err.message.c_str();

        result = "{\"code\":" + code + ",\"message\":\"" + message + "\"}";
    } catch (const std::exception &err) {
        printf("[ValhallaActor] route std::exception: %s\n", err.what());
        result = "{\"code\":-1,\"message\":\"" + std::string(err.what()) + "\"}";
    } catch (...) {
        printf("[ValhallaActor] route unknown exception");
        result = "{\"code\":-1,\"message\":\"unknown exception\"}";
    }

    env->ReleaseStringUTFChars(jRequest, request);
    env->ReleaseStringUTFChars(jConfigPath, config_path);

    return env->NewStringUTF(result.c_str());
}

#elif __APPLE__
void* create_valhalla_actor(const char *config_path, ValhallaMobileHttpClient* http_client) {
    return new ValhallaActor(config_path, http_client);
}

void delete_valhalla_actor(void* actor) {
    delete ((ValhallaActor*) actor);
}

// Generic dispatcher used by every action's bridge function below —
// each one only differs in which `ValhallaActor` method it calls and
// the action label used in error logs. Centralizing the try/catch
// keeps the file shorter and ensures every action returns the same
// `{"code":N,"message":"..."}` envelope on failure (matching the
// shape that Rallista's Swift `route(request:)` decoder already
// recognizes via `ValhallaErrorModel`).
template<typename Fn>
static std::string dispatch_action(const char* action_name, Fn&& fn) {
    std::string result;
    try {
        result = fn();
    } catch (const valhalla::valhalla_exception_t &err) {
        printf("[ValhallaActor] %s valhalla_exception: %s\n", action_name, err.what());
        std::string code = std::to_string(err.code);
        std::string message = err.message.c_str();
        result = "{\"code\":" + code + ",\"message\":\"" + message + "\"}";
    } catch (const std::exception &err) {
        printf("[ValhallaActor] %s std::exception: %s\n", action_name, err.what());
        result = "{\"code\":-1,\"message\":\"" + std::string(err.what()) + "\"}";
    } catch (...) {
        printf("[ValhallaActor] %s unknown exception\n", action_name);
        result = "{\"code\":-1,\"message\":\"unknown exception\"}";
    }
    return result;
}

std::string route(const char *request, void* actor) {
    return dispatch_action("route", [&]() {
        return ((ValhallaActor*) actor)->route(request);
    });
}

std::string trace_route(const char *request, void* actor) {
    return dispatch_action("trace_route", [&]() {
        return ((ValhallaActor*) actor)->trace_route(request);
    });
}

std::string trace_attributes(const char *request, void* actor) {
    return dispatch_action("trace_attributes", [&]() {
        return ((ValhallaActor*) actor)->trace_attributes(request);
    });
}

std::string locate(const char *request, void* actor) {
    return dispatch_action("locate", [&]() {
        return ((ValhallaActor*) actor)->locate(request);
    });
}

std::string optimized_route(const char *request, void* actor) {
    return dispatch_action("optimized_route", [&]() {
        return ((ValhallaActor*) actor)->optimized_route(request);
    });
}
#endif
