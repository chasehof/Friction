#include <atomic>
#include <array>

/**
 * Single producer, single consumer lock-free queue.
 * The queue is implemented as a circular buffer with a fixed size of N.
*/

template <typename T, std::size_t N>
class LockFreeQueue{
public: 

LockFreeQueue();

void push(const T& item);

bool pop(T& out);

std::size_t size() const;

private:

std::atomic<std::size_t> head;
std::atomic<std::size_t> tail;
T std::array<T, N> buffer;

};