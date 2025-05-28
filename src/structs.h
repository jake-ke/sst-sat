#ifndef structs_h
#define structs_h

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

// Comparator for the variable activity heap
struct VarOrderLt {
    const std::vector<double>& activity;
    
    VarOrderLt(const std::vector<double>& act) : activity(act) {}
    
    bool operator()(int x, int y) const {
        return activity[x] > activity[y];  // Higher activity first
    }
};

#endif // structs_h