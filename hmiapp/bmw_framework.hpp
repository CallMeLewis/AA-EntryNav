// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Stand-ins for BMW's RHMI app framework (libAppFramework.A.so, libCoreFramework.A.so), and the few tricks the app
// needs on top of it: its own bmw::Runnable, a bmw::PluginApp whose vtable calls the app's onInit/onError/onFinalize.
//
// No BMW header exists here: only names and parameter types matter, they give the mangled symbols the car's libraries
// export (tests/test_hmiapp_load.cpp loads the app against them). Facts read on the copy of the car's system:
//   - bmw::PluginApp's primary vtable has 34 slots: onError 21, onInit 22, onFinalize 23 (a secondary
//     PluginAppHandler vtable follows); its constructor writes up to +0x174, within kPluginAppStorage.
//   - bmw::Runnable has no data member: its destructor only resets the vtable pointer (libCoreFramework). The
//     framework keeps the Runnable's address in a Message and calls run() through that pointer.
#ifndef AA_ENTRYNAV_HMIAPP_BMW_FRAMEWORK_HPP
#define AA_ENTRYNAV_HMIAPP_BMW_FRAMEWORK_HPP

#include <stddef.h>

namespace bmw {
class IDispatcher;
class Bundle;
class Runnable;
enum RHMIInterfaceVersion { RHMI_INTERFACE_VERSION_1 = 1 };
template <typename T>
class BasicString {
public:
    BasicString(const T* s);                // _ZN3bmw11BasicStringIcEC1EPKc, ...ItEC1EPKt
    BasicString(const BasicString& other);  // _ZN3bmw11BasicStringIcEC1ERKS1_
    ~BasicString();                         // _ZN3bmw11BasicStringIcED1Ev, ...ItED1Ev
    const T* raw() const { return m_data; }

private:
    T* m_data;
};
class Parcel {
public:
    explicit Parcel(bool value);       // _ZN3bmw6ParcelC1Eb
    ~Parcel();                         // _ZN3bmw6ParcelD1Ev
    void readBool(bool& value) const;  // _ZNK3bmw6Parcel8readBoolERb
    // A string an action sends (the speller's text): big-endian u16 length, then big-endian UTF-16.
    void readWString(BasicString<unsigned short>& value) const;  // _ZNK3bmw6Parcel11readWStringERNS_11BasicStringItEE
private:
    unsigned char m_storage[32];
};
// libCoreFramework's in-memory stream (0x18 bytes), held by an Image.
class BufferStream {
public:
    ~BufferStream();  // _ZN3bmw12BufferStreamD1Ev
private:
    unsigned char m_storage[0x18];
};
// A picture handed to a raImageModel: a whole PNG (or JPEG, BMP) file, 1 to 1,024,000 bytes, recognised by its first
// bytes. The constructor copies the bytes. No destructor is exported: only its members' run (0x20 bytes in all).
class Image {
public:
    Image(const unsigned char* data, unsigned size);  // _ZN3bmw5ImageC1EPKhj
    ~Image() {}

private:
    BasicString<char> m_path;
    BufferStream m_stream;
    bool m_loaded;
};
class TextID {
public:
    explicit TextID(unsigned id);  // _ZN3bmw6TextIDC1Ej
private:
    unsigned m_id, m_type;
};
class ImageID {
public:
    explicit ImageID(unsigned id);  // _ZN3bmw7ImageIDC1Ej
private:
    unsigned m_id, m_type;
};
// A value of an action's parameter map: one pointer to a holder (vtable, then the value at +4), null when empty.
// type() asks the holder (HmiMain3D's Variant::ObjectContainer<T>::type()): 0x25 for BasicString<unsigned short>, 0x22
// for BasicString<char>, 0x10 int, 0x13 bool, 0x19 float; 0 when empty.
class Variant {
public:
    unsigned type() const;  // _ZNK3bmw7Variant4typeEv
    const void* holder() const { return m_holder; }

private:
    void* m_holder;
};
const unsigned kVariantWideString = 0x25;
// libCoreFramework's hash map. Only its operator[] is exported for Variant values; it adds the key when missing.
template <typename K, typename V>
class Map {
public:
    V& operator[](const K& key);  // _ZN3bmw3MapIjNS_7VariantEEixERKj
private:
    unsigned char m_storage[0x20];
};
template <typename R, typename A1, typename A2, typename A3, typename A4>
struct Delegate {
    void* object;
    R (*stub)(void* object, A1 a1);
};
using ActionDelegate = Delegate<bool, const Parcel&, void, void, void>;
using MapActionDelegate = Delegate<bool, const Map<unsigned, Variant>&, void, void, void>;
using CdsDelegate = Delegate<bool, const BasicString<unsigned short>&, void, void, void>;
const size_t kPluginAppStorage = 0x200;  // EntryEVO constructor writes up to +0x174
class PluginApp {
public:
    PluginApp(IDispatcher& dispatcher, BasicString<char> name, unsigned major, unsigned minor,
              RHMIInterfaceVersion rhmi, const Bundle& params, const Bundle& config);
    void addHandler(unsigned actionId, const ActionDelegate& handler);
    // An action's parameter map, which the Parcel of addHandler does not carry: with asyncCallbacks (the manifest),
    // the HMI puts the parameters in the message's map, and the Parcel holds the action's id only.
    // _ZN3bmw9PluginApp17addHandlerWithMapEjRKNS_8DelegateIbRKNS_3MapIjNS_7VariantEEEvvvEE
    void addHandlerWithMap(unsigned actionId, const MapActionDelegate& handler);
    void addHandler(unsigned id, unsigned type, const ActionDelegate& handler);
    void triggerEvent(unsigned eventId);
    void triggerEvent(unsigned eventId, const Parcel& value);  // _ZN3bmw9PluginApp12triggerEventEjRKNS_6ParcelE
    void post(Runnable* runnable, unsigned delayMs);
    void updateModel(unsigned modelId, const TextID& text);
    void updateModel(unsigned modelId, const ImageID& image);
    void updateModel(unsigned modelId, bool value);
    void updateModel(unsigned modelId, const char* text);    // _ZN3bmw9PluginApp11updateModelEjPKc (raDataModel)
    void updateModel(unsigned modelId, const Image& image);  // _ZN3bmw9PluginApp11updateModelEjRKNS_5ImageE
    void setProperty(unsigned componentId, unsigned propertyId, const Parcel& value);
    void cdsAddPropertyChangedEventHandler(BasicString<char>& property, unsigned id, const CdsDelegate& handler);
    bool onInit();
    void onFinalize();

private:
    unsigned char m_storage[kPluginAppStorage];
};
}  // namespace bmw

namespace evo {
namespace app {

// ---- tasks on the app thread ---------------------------------------------------------------------------------------
// A bmw::Runnable of our own: BMW's vtable (offset, typeinfo, D1, D0, run) copied, destructors made no-ops, run() ours.
struct AppRunnable {
    void** vptr;  // where a bmw::Runnable keeps its vtable pointer, and all it holds
    void* vtbl[5];
};
void makeRunnable(AppRunnable& r, void (*run)(void*));
void postRunnable(bmw::PluginApp* app, AppRunnable& r, unsigned delayMs);

// ---- handlers ------------------------------------------------------------------------------------------------------
typedef bool (*ActionStub)(void* app, const bmw::Parcel& value);
typedef bool (*MapActionStub)(void* app, const bmw::Map<unsigned, bmw::Variant>& params);
typedef bool (*CdsStub)(void* app, const bmw::BasicString<unsigned short>& value);
void addAction(bmw::PluginApp* app, unsigned actionId, ActionStub stub);
void addMapAction(bmw::PluginApp* app, unsigned actionId, MapActionStub stub);
void addStateHandler(bmw::PluginApp* app, unsigned state, unsigned kind, ActionStub stub);
void addCdsHandler(bmw::PluginApp* app, const char* property, CdsStub stub);

bool parcelBool(const bmw::Parcel& value);
// The string an action sent, as ASCII ('?' for anything else), cut to cap - 1 characters.
void parcelText(const bmw::Parcel& value, char* text, size_t cap);
// The parameter under key as ASCII, the same way, if it is a UTF-16 string; *type gets its Variant type (0: none).
// The speller's OK sends the text under key 8 (CAbstractInputHmiState::spellerOkPressed).
bool mapText(const bmw::Map<unsigned, bmw::Variant>& params, unsigned key, char* text, size_t cap, unsigned* type);
const unsigned kSpellerTextKey = 8;
// A whole PNG file to a raImageModel.
void showImage(bmw::PluginApp* app, unsigned model, const unsigned char* png, size_t size);
// A CDS text (UTF-16) as ASCII, '?' for anything else, cut to cap - 1 characters.
void narrow(const bmw::BasicString<unsigned short>& value, char* text, size_t cap);

// ---- the app object ------------------------------------------------------------------------------------------------
struct PluginAppHooks {
    bool (*onInit)(bmw::PluginApp* app);
    void (*onError)(bmw::PluginApp* app, unsigned code, const bmw::BasicString<char>& text);
    void (*onFinalize)(bmw::PluginApp* app);
};
// A bmw::PluginApp in storage of its own, given a copy of its primary vtable with the three hooks in place.
// *bmwVtable: the vtable BMW's constructor set, for the log.
bmw::PluginApp* newPluginApp(bmw::IDispatcher& dispatcher, const char* name, const bmw::Bundle& params,
                             const bmw::Bundle& config, const PluginAppHooks& hooks, void** bmwVtable);
// Its deleting destructor (vtable slot 1).
void deletePluginApp(void* app);

}  // namespace app
}  // namespace evo

#endif
