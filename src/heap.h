#ifndef HEAP_H
#define HEAP_H

#include <vector>
#include <cassert>

// Generic heap implementation for ordering elements with a given comparator
template<class K, class Comp>
class Heap {
private:
    std::vector<K> heap;     // Heap of keys
    std::vector<int> indices;  // Map elements to their heap positions
    Comp lt;                   // The heap comparator

    // Sift element at position i up/down in the heap
    void percolateUp(int i) {
        K x = heap[i];
        int p = (i - 1) >> 1;
        
        while (i > 0 && lt(x, heap[p])) {
            heap[i] = heap[p];
            indices[heap[p]] = i;
            i = p;
            p = (p - 1) >> 1;
        }
        
        heap[i] = x;
        indices[x] = i;
    }

    void percolateDown(int i) {
        K x = heap[i];
        while (i < (int)(heap.size()/2)) {
            int child = (i << 1) + 1;
            if (child + 1 < (int)heap.size() && lt(heap[child+1], heap[child]))
                child++;
            if (!lt(heap[child], x)) break;
            heap[i] = heap[child];
            indices[heap[i]] = i;
            i = child;
        }
        
        heap[i] = x;
        indices[x] = i;
    }

public:
    Heap(const Comp& c) : lt(c) {}
    
    // Accessors
    bool empty() const { return heap.empty(); }
    int size() const { return heap.size(); }
    bool inHeap(K n) const { return n < (int)indices.size() && indices[n] >= 0; }
    int operator[](int index) const { assert(index < heap.size()); return heap[index]; }
    void decrease  (K k) { assert(inHeap(k)); percolateUp  (indices[k]); }
    void increase  (K k) { assert(inHeap(k)); percolateDown(indices[k]); }
    
    // Operations
    void clear() { 
        for (size_t i = 0; i < heap.size(); i++)
            indices[heap[i]] = -1;
        heap.clear(); 
    }

    void insert(K n) {
        if (n >= (int)indices.size())
            indices.resize(n+1, -1);
        assert(!inHeap(n));
        
        indices[n] = heap.size();
        heap.push_back(n);
        percolateUp(indices[n]);
    }

    K removeMin() {
        assert(!heap.empty());
        // bring the last element to the root and percolate down
        K x = heap[0];
        heap[0] = heap.back();
        indices[heap[0]] = 0;
        indices[x] = -1;
        heap.pop_back();
        if (heap.size() > 1) 
            percolateDown(0);
        return x;
    }

    // Build the heap from an array of elements
    void build(const std::vector<K>& ns) {
        clear();
        for (size_t i = 0; i < ns.size(); i++)
            insert(ns[i]);
    }
};

#endif // HEAP_H
