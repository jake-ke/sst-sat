#!/usr/bin/env python3
"""
SAT Solution Verifier

This script verifies if a given solution satisfies all clauses in a DIMACS CNF file.
It takes a solution file and a CNF file as input and reports unsatisfied clauses.

Usage:
    python verifier.py <solution_file> <cnf_file>
    
Solution file format:
    Single line with assignments: x1=1 x2=0 x3=1 ...
    where value is 0 (false) or 1 (true)
    
CNF file format:
    Standard DIMACS format
"""

import sys
import os
from typing import Dict, List, Set


def parse_solution(solution_file: str) -> Dict[int, bool]:
    """
    Parse the solution file and return a dictionary mapping variables to their truth values.
    
    Args:
        solution_file: Path to the solution file
        
    Returns:
        Dictionary mapping variable numbers to boolean values
    """
    assignment = {}
    
    try:
        with open(solution_file, 'r') as f:
            content = f.read().strip()
            
            # Handle format: x1=1 x2=1 x3=0 ...
            # Split by spaces and parse each assignment
            parts = content.split()
            for part in parts:
                if '=' in part and part.startswith('x'):
                    try:
                        # Extract variable number and value from format "x123=0"
                        var_part, value_part = part.split('=')
                        var = int(var_part[1:])  # Remove 'x' prefix
                        value = int(value_part)
                        
                        if value not in [0, 1]:
                            print(f"Warning: Invalid value {value} for variable {var}, expected 0 or 1")
                            continue
                        
                        assignment[var] = bool(value)
                        
                    except (ValueError, IndexError):
                        print(f"Warning: Invalid assignment format: {part}")
                        continue
                    
    except FileNotFoundError:
        print(f"Error: Solution file '{solution_file}' not found")
        sys.exit(1)
    except Exception as e:
        print(f"Error reading solution file: {e}")
        sys.exit(1)
    
    return assignment


def parse_cnf(cnf_file: str) -> tuple[List[List[int]], int, int]:
    """
    Parse the DIMACS CNF file and return clauses, number of variables, and number of clauses.
    
    Args:
        cnf_file: Path to the CNF file
        
    Returns:
        Tuple of (clauses, num_variables, num_clauses)
        where clauses is a list of lists containing the literals in each clause
    """
    clauses = []
    num_vars = 0
    num_clauses = 0
    
    try:
        with open(cnf_file, 'r') as f:
            for line_num, line in enumerate(f, 1):
                line = line.strip()
                
                # Skip empty lines and comments
                if not line or line.startswith('c'):
                    continue
                
                # Parse problem line
                if line.startswith('p cnf'):
                    parts = line.split()
                    if len(parts) != 4:
                        print(f"Error: Invalid problem line format on line {line_num}")
                        sys.exit(1)
                    num_vars = int(parts[2])
                    num_clauses = int(parts[3])
                    continue
                
                # Parse clause
                try:
                    literals = [int(x) for x in line.split()]
                    
                    # Check if clause ends with 0
                    if literals and literals[-1] == 0:
                        literals = literals[:-1]  # Remove the trailing 0
                    
                    if literals:  # Only add non-empty clauses
                        clauses.append(literals)
                        
                except ValueError:
                    print(f"Warning: Invalid clause format on line {line_num}: {line}")
                    continue
                    
    except FileNotFoundError:
        print(f"Error: CNF file '{cnf_file}' not found")
        sys.exit(1)
    except Exception as e:
        print(f"Error reading CNF file: {e}")
        sys.exit(1)
    
    return clauses, num_vars, num_clauses


def evaluate_clause(clause: List[int], assignment: Dict[int, bool]) -> bool:
    """
    Evaluate a single clause given the variable assignment.
    
    Args:
        clause: List of literals (positive for variable, negative for negated variable)
        assignment: Dictionary mapping variables to their truth values
        
    Returns:
        True if the clause is satisfied, False otherwise
    """
    for literal in clause:
        var = abs(literal)
        
        # If variable is not assigned, we can't evaluate the clause properly
        if var not in assignment:
            print(f"Warning: Variable {var} not found in assignment")
            continue
        
        # Check if literal is satisfied
        if literal > 0:  # Positive literal
            if assignment[var]:
                return True
        else:  # Negative literal
            if not assignment[var]:
                return True
    
    return False


def verify_solution(solution_file: str, cnf_file: str) -> bool:
    """
    Verify if the solution satisfies all clauses in the CNF file.
    
    Args:
        solution_file: Path to the solution file
        cnf_file: Path to the CNF file
        
    Returns:
        True if all clauses are satisfied, False otherwise
    """
    print(f"Verifying solution '{solution_file}' against CNF '{cnf_file}'")
    print("=" * 60)
    
    # Parse files
    assignment = parse_solution(solution_file)
    clauses, num_vars, num_clauses = parse_cnf(cnf_file)
    
    print(f"Loaded {len(assignment)} variable assignments")
    print(f"CNF file has {num_vars} variables and {num_clauses} clauses")
    print(f"Actually parsed {len(clauses)} clauses")
    print()
    
    # Verify each clause
    unsatisfied_clauses = []
    
    for i, clause in enumerate(clauses):
        if not evaluate_clause(clause, assignment):
            unsatisfied_clauses.append((i, clause))
    
    # Report results
    if not unsatisfied_clauses:
        print("✓ All clauses are satisfied! The solution is VALID.")
        return True
    else:
        print(f"✗ {len(unsatisfied_clauses)} clause(s) are NOT satisfied:")
        print()
        
        for clause_num, clause in unsatisfied_clauses:
            print(f"Clause {clause_num}: {' '.join(map(str, clause))}")
            
            # Show why the clause is unsatisfied
            literal_values = []
            for literal in clause:
                var = abs(literal)
                if var in assignment:
                    var_value = assignment[var]
                    if literal > 0:
                        literal_values.append(f"{literal}={var_value}")
                    else:
                        literal_values.append(f"{literal}={not var_value}")
                else:
                    literal_values.append(f"{literal}=?")
            
            print(f"  Literal evaluations: {', '.join(literal_values)}")
            print()
        
        return False


def main():
    """Main function."""
    if len(sys.argv) != 3:
        print("Usage: python verifier.py <solution_file> <cnf_file>")
        print()
        print("Example:")
        print("  python verifier.py examples/4_4_3_decisions.txt examples/cmin_eg.dimacs")
        sys.exit(1)
    
    solution_file = sys.argv[1]
    cnf_file = sys.argv[2]
    
    # Check if files exist
    if not os.path.exists(solution_file):
        print(f"Error: Solution file '{solution_file}' does not exist")
        sys.exit(1)
    
    if not os.path.exists(cnf_file):
        print(f"Error: CNF file '{cnf_file}' does not exist")
        sys.exit(1)
    
    # Verify the solution
    is_valid = verify_solution(solution_file, cnf_file)
    
    # Exit with appropriate code
    sys.exit(0 if is_valid else 1)


if __name__ == "__main__":
    main()
