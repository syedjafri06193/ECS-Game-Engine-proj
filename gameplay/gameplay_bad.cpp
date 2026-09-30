// A module that breaks the contract on purpose. tools/module_verify.py must
// reject it (the test suite checks that it does). Never loaded.
#include "modules/module_sdk.h"

namespace {
int frame_counter = 0;                     // persistent state: lost/duplicated on reload
}  // namespace

// Virtual functions on a module object: the vtable lives in module memory
// and dangles after unload. The out-of-line key function guarantees the
// vtable is emitted even at -O3.
struct Behaviour {
    virtual ~Behaviour();
    virtual void Tick();
};
Behaviour::~Behaviour() = default;
void Behaviour::Tick() { ++frame_counter; }

ENGINE_MODULE_EXPORT void System_Count(const EngineHostApi*, const EngineChunk*, float, void*)
{
    static Behaviour* b = new Behaviour;   // function-local static + heap object owned by the module
    b->Tick();
}

ENGINE_MODULE_EXPORT int HelperNotASystem() { return frame_counter; }   // unexpected export

namespace {
constexpr const char* kNone[] = {nullptr};
constexpr EngineSystemDesc kSystems[] = {{"count", kNone, kNone, 0, "System_Count", 256}};
constexpr EngineManifest kManifest = {ENGINE_MODULE_ABI_VERSION, "bad", 1, 1, kSystems};
}  // namespace

ENGINE_MODULE_EXPORT const EngineManifest* EngineModuleManifest() { return &kManifest; }
