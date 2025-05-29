#ifndef structs_h
#define structs_h

using coro_t = boost::coroutines2::coroutine<void>;

// Define types for variables and literals
typedef int Var;
const Var var_Undef = 0;

// Define a constant for undefined clause reference
const int ClauseRef_Undef = -1;

struct Lit {
    int x;
    
    bool operator == (const Lit& other) const { return x == other.x; }
    bool operator != (const Lit& other) const { return x != other.x; }
    bool operator <  (const Lit& other) const { return x < other.x; }
};
const Lit lit_Undef = { 0 }; // Special undefined literal

struct Variable {
    size_t level;   // Decision level when variable was assigned
    int reason;     // Index of clause that caused this assignment
    
    Variable() : level(0), reason(ClauseRef_Undef) {}
};

// Comparator for the variable activity heap
struct VarOrderLt {
    const std::vector<double>& activity;
    
    VarOrderLt(const std::vector<double>& act) : activity(act) {}
    
    bool operator()(int x, int y) const {
        return activity[x] > activity[y];  // Higher activity first
    }
};

#endif // structs_h