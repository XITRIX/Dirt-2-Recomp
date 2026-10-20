#pragma once
// Application-visible COM references and device/state ownership are distinct.
// All calls are serialized by ApiLock in the interface implementation.
namespace deko {
class ObjectRefs {
public:
    unsigned add_ref() {
        if (public_++ == 0 && parent_) parent_->AddRef();
        return public_;
    }
    template<class T> unsigned release(T* object) {
        const unsigned remaining=--public_;
        if (!remaining) {
            // Parent destruction can release private bindings and delete object.
            // Snapshot it and do not touch object after releasing that parent.
            auto* parent=parent_;
            if (!private_) delete object;
            if (parent) parent->Release();
        }
        return remaining;
    }
    void add_private() { ++private_; }
    template<class T> void release_private(T* object) {
        if (!--private_ && !public_) delete object;
    }
    void set_parent(IUnknown* parent) {
        parent_=parent;
        if (public_ && parent_) parent_->AddRef();
    }
private:
    unsigned public_=1,private_=0;
    IUnknown* parent_=nullptr;
};
}
