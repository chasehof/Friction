#include <atomic>

/**
 * Single producer, single consumer lock-free queue.
 * The queue is implemented as a circular buffer with a fixed size of N.
*/

template <typename T, std::size_t N>
class LockFreeQueue{
public: 

LockFreeQueue();
void push();

private:

};