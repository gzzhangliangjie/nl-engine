// P8 M-D1: ContactEventSystem implementation. See header for the owner->Entity
// filter and the self-contained (empty reads/writes) parallel-tick contract.
#include "nl/physics/ContactEventSystem.hpp"

namespace nl::physics {

void ContactEventSystem::tick(double /*dt*/) {
    ++stats_.ticks;
    lastEvents_ = stream_.drain();
    stats_.lastFrameEvents = lastEvents_.size();
    for (const ContactEvent& e : lastEvents_) {
        ++stats_.events;
        if (world_.exists(static_cast<nl::scene::EntityID>(e.ownerA))) ++stats_.mappedA;
        else ++stats_.unmappedA;
        if (world_.exists(static_cast<nl::scene::EntityID>(e.ownerB))) ++stats_.mappedB;
        else ++stats_.unmappedB;
    }
}

} // namespace nl::physics
