// P8 M-D1: ContactEventStream implementation. See header for the three-source
// evidence and the update/publish/drain contract.
#include "nl/physics/ContactEventStream.hpp"

namespace nl::physics {

std::size_t ContactEventStream::unreadIn(int page) const {
    const std::size_t n = pages_[page].size();
    const std::size_t c = cursors_[page];
    return n > c ? n - c : 0;
}

void ContactEventStream::appendUnread(int page, std::vector<ContactEvent>* out) {
    const std::size_t n = pages_[page].size();
    for (std::size_t i = cursors_[page]; i < n; ++i) {
        out->push_back(pages_[page][i]);
    }
    cursors_[page] = n;
}

std::size_t ContactEventStream::update() {
    const int older = 1 - write_;
    const std::size_t dropped = unreadIn(older);
    droppedTotal_ += dropped;
    // The oldest page is recycled as the new write page. The page we just
    // finished writing becomes the readable older page (two-frame survival).
    pages_[older].clear();
    cursors_[older] = 0;
    write_ = older;
    ++updateCount_;
    return dropped;
}

void ContactEventStream::publish(const ContactEvent& e) {
    pages_[write_].push_back(e);
    ++publishedTotal_;
}

void ContactEventStream::publish(const std::vector<ContactEvent>& events) {
    pages_[write_].insert(pages_[write_].end(), events.begin(), events.end());
    publishedTotal_ += events.size();
}

std::vector<ContactEvent> ContactEventStream::drain() {
    std::vector<ContactEvent> out;
    out.reserve(unreadIn(1 - write_) + unreadIn(write_));
    appendUnread(1 - write_, &out);   // older (previous frame) first
    appendUnread(write_, &out);       // current frame second
    drainedTotal_ += out.size();
    return out;
}

std::size_t ContactEventStream::readableCount() const {
    return unreadIn(0) + unreadIn(1);
}

std::size_t ContactEventStream::pendingCount() const {
    return pages_[write_].size();
}

void ContactEventStream::clear() {
    pages_[0].clear();
    pages_[1].clear();
    cursors_[0] = cursors_[1] = 0;
    write_ = 0;
    droppedTotal_ = 0;
    updateCount_ = 0;
    publishedTotal_ = 0;
    drainedTotal_ = 0;
}

} // namespace nl::physics
