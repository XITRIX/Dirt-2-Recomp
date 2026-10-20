#include "../platform.h"
#include <SDL3/SDL.h>
#include <jni.h>

namespace platform {
namespace {
jstring str(JNIEnv* env, const std::string& s) { return env->NewStringUTF(s.c_str()); }
}
bool prompt_text(const std::string& title, const std::string& message,
                 const std::string& initial, std::string& result) {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity) return false;
    jclass cls = env->GetObjectClass(activity);
    jmethodID method = env->GetMethodID(cls, "promptText", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    jstring t = str(env,title), m = str(env,message), i = str(env,initial);
    auto answer = method ? static_cast<jstring>(env->CallObjectMethod(activity,method,t,m,i)) : nullptr;
    bool ok = answer && !env->ExceptionCheck();
    if (ok) {
        const char* utf = env->GetStringUTFChars(answer,nullptr);
        if (utf) { result = utf; env->ReleaseStringUTFChars(answer,utf); } else ok = false;
    }
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
    if(answer) env->DeleteLocalRef(answer);
    env->DeleteLocalRef(t); env->DeleteLocalRef(m); env->DeleteLocalRef(i);
    env->DeleteLocalRef(cls); env->DeleteLocalRef(activity);
    return ok;
}
int choose(const std::string& title, const std::string& message,
           const std::string& first, const std::string& second) {
    SDL_MessageBoxButtonData buttons[] = {{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT,0,first.c_str()},
                                         {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT,1,second.c_str()}};
    SDL_MessageBoxData data{SDL_MESSAGEBOX_INFORMATION,nullptr,title.c_str(),message.c_str(),2,buttons,nullptr};
    int answer=1;
    SDL_ShowMessageBox(&data,&answer);
    return answer;
}
bool choose_folder(const std::string&, std::string&) { return false; }
void set_app_icon(const std::string&) {}
void cloud_saves_start(const std::string&) {}
void cloud_saves_flush() {}
void metalfx_init() {}
bool metalfx_supported() { return false; }
bool metalfx_enabled() { return false; }
void set_metalfx_enabled(bool) {}
}
