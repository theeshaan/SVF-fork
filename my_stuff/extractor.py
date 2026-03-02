# A utility to parse SVF output and print source variables' points-to information on the terminal
import argparse
import re
from collections import defaultdict
from pathlib import Path
import subprocess
import shutil
import copy

def parse_pta_input(file_path):
    # token map gives info about a token given the token's id (info = (name, file, line))
    # graph is the actual points-to graph
    token_map = {}
    graph = defaultdict(set)

    with open(file_path, 'r') as file:
        content = file.read()

    matches = re.findall(r"##<(?P<name>.+)> Source Loc: { (.*)?\"ln\": (?P<line>\d+),.* \"(file|fl)\": \"(?P<file>.+)\" }\nPtr (\d+)", content)
    
    for match in matches:
        name, _, line, _, fl, node_id = match
        token_map[int(node_id)] = (name, fl, line)

    # TODO: This regex works for the cases where context is printed as empty "[: ]"
    # modify it to work with contexts as well
    matches = re.findall(r"<(\d+)\s*\[:\s*\]>\s*==>\s*\{\s*((?:<\d+\s*\[:\s*\]>\s*)*)\}", content)

    for lhs, rhs in matches:
        graph[int(lhs)] = {int(x) for x in re.findall(r"<(\d+)", rhs)}

    # print("Before processing PTA:")
    # for k, v in graph.items():
    #     print(f"Node {k}: {v}")

    # Parse the PTA, specifically store edges, to match pointers to variables stored in them
    matches = re.findall(r"(\d+)\s*-- Store -->\s*(\d+)", content)

    temp_graph = copy.deepcopy(graph)

    for match in matches:
        source, variable = match # the source is being stored into the variable
        source = int(source)
        variable = int(variable)
        if variable not in graph:
            graph[variable] = set()
            temp_graph[variable] = set()
        if source not in graph:
            graph[source] = set()
            temp_graph[source] = set()
        # If the variable currently points to only one node and that node is the same as the variable
        # then we can just update the variable's pointee set to the source's pointee set
        if len(temp_graph[variable]) == 1 and token_map[list(temp_graph[variable])[0]][0] == token_map[variable][0]:
            temp_graph[variable] = graph[source].copy()
        else:
            # TODO: Check how the existing pointee set of a variable should be merged with
            # the new info we get from here (for now I'm just appending to the existing info)
            temp_graph[variable].update(graph[source])
    graph = temp_graph

    # print("After processing PTA:")
    # for k, v in graph.items():
    #     print(f"Node {k}: {v}")

    # # TODO: VERY IMPORTANT: check if below line (deleting the source from the graph)
    # # is correct
    # for match in matches:
    #     source,variable = match
    #     if source in graph:
    #         graph.pop(source)

    return graph, token_map

def demangle(name):
    # Check if c++filt is available
    if shutil.which("c++filt"):
        try:
            # Run c++filt and get the output
            result = subprocess.run(["c++filt", "-n", name], capture_output=True, text=True, check=True)
            demangled = result.stdout.strip()
            # the result after demangling will be like f(int,int). We want just the name of the function.
            return demangled.split("(")[0] # Taking just the name part before args
        except subprocess.CalledProcessError:
            return name
    else:
        print("c++filt not found. Function names may be mangled.")
    return name

def parse_callgraph_dot(dot_file_path):
    func_map = {}

    # Regex for CallGraphNode in callgraph_final.dot
    node_re = re.compile(r'Node(?P<id>0x[0-9a-f]+)\s+\[.*?label="\{CallGraphNode ID: (?P<node_id>\d+) \\{fun: (?P<func_name>.*?)\\}.*?"\];')
    
    if not Path(dot_file_path).exists():
        return {}

    with open(dot_file_path, 'r') as f:
        for line in f:
            node_match = node_re.search(line)
            if node_match:
                func_name = node_match.group("func_name")
                demangled_name = demangle(func_name)
                # Map mangled name to demangled name
                func_map[func_name] = demangled_name

    return func_map


def remove_dead_nodes(graph, token_map):
    # Pass 1: Bridge hidden nodes (nodes not in token_map)
    # If A -> B -> C and B is hidden (not in token_map), make A -> C.
    
    # We need a fixpoint loop because we might have chains of hidden nodes
    change = True
    while change:
        change = False
        hidden_nodes = [n for n in graph.keys() if n not in token_map]
        hidden_set = set(hidden_nodes) # fast look up
        
        # Build reverse graph for efficient parent lookup
        # We only care about edges pointing TO hidden nodes
        parents = defaultdict(set)
        for src, dests in graph.items():
            for dst in dests:
                if dst in hidden_set:
                    parents[dst].add(src)

        for hidden in hidden_nodes:
            # If hidden node has parents, propagate its children to them
            if hidden in parents:
                children = graph[hidden]
                for p in parents[hidden]:
                    if hidden in graph[p]: # Check existence to be safe
                        graph[p].remove(hidden)
                        graph[p].update(children)
                        change = True
            
            # Remove hidden node from graph definition
            if hidden in graph: 
                graph.pop(hidden)
                # Removing a hidden node is a change
                change = True

    # Pass 2: Clean up leaf nodes (source nodes that point to nothing)
    # and "same token" reduction.
    change = True
    while change:
        change = False
        keys = list(graph.keys()) # Copy keys
        for node in keys:
            # Case 1: Empty pointers (Leaf nodes)
            if len(graph[node]) == 0:
                graph.pop(node)
                change = True
                continue
            
            # Case 2: Node points only to itself/same-token (Redundant)
            # Logic: if A -> {B} and Token(A) == Token(B), remove A.
            if len(graph[node]) == 1:
                other = list(graph[node])[0]
                if other in token_map and node in token_map and token_map[node] == token_map[other]:
                    graph.pop(node)
                    change = True
                    continue
            
            # Case 3: Prune edges to non-existent nodes (should be handled by bridging, but for safety)
            # Actually, bridging restricted itself to `graph.keys()`. 
            # If there are pointees that are NOT in graph.keys() AND NOT in token_map, they are dead ends.
            current_pointees = list(graph[node])
            for p in current_pointees:
                if p not in token_map and p not in graph:
                    graph[node].remove(p)
                    change = True

def remap_nodes(graph, token_map):
    node_mapping = {node: idx + 3 for idx, node in enumerate(sorted(token_map.keys()))}
    
    new_token_map = {idx: token_map[node] for node, idx in node_mapping.items()}

    
    new_graph = defaultdict(set)
    for ptr, points in graph.items():
        if ptr not in node_mapping:
            continue
        mapped_ptr = node_mapping[ptr]
        mapped_points = {node_mapping[p] for p in points if p in node_mapping}
        new_graph[mapped_ptr] = mapped_points

    return new_graph, new_token_map

def map_variables_to_functions(token_map, func_name_map):
    # How the function works:
    # - I found that all functions also have dummy points-to entries in the SVF output.
    # - Along with these dummy entries, the line numbers on which they are defined are specified.
    # - So the line numbers at which a function starts are extracted from the points to output
    # - then I check between which 2 functions a variable has been defined, and assign it as belonging to the upper function.
    # - TODO: Check if this breaks for corner cases, eg function declared at the top but defined later. Also check global varibles
    # - Update: checked, it works.

    # 1. Identify function start lines
    func_starts = []
    seen_funcs = set()
    
    sorted_tokens = sorted(token_map.items(), key=lambda x: int(x[1][2])) # Sort by line

    for _, (name, file, line) in sorted_tokens:
        # Check if the token name is one of the known function names (mangled)
        if name in func_name_map:
            line_int = int(line)
            # Use demangled name for later suffixing? No, we store the demangled name in the entry
            demangled_name = func_name_map[name]
            entry = (file, line_int, demangled_name)
            if entry not in seen_funcs:
                func_starts.append(entry)
                seen_funcs.add(entry)
    
    func_starts.sort() # Sort by file, then line
    
    funcs_by_file = defaultdict(list)
    for f, l, n in func_starts:
        funcs_by_file[f].append((l, n))

    new_token_map = {}
    
    for node_id, (name, file, line) in token_map.items():
        line_int = int(line)
        
        # Don't suffix functions themselves
        if name in func_name_map:
             # But maybe we want the demangled name in the output?
             # "Pointer p_main" is good. "Pointer f" is better than "Pointer _Z1fv".
             # Let's clean up function names too.
             demangled = func_name_map[name]
             new_token_map[node_id] = (demangled, file, line)
             continue
             
        target_func = None
        if file in funcs_by_file:
            candidates = funcs_by_file[file]
            for f_start, f_name in candidates:
                if f_start <= line_int:
                    target_func = f_name
                else:
                    break 
        
        if target_func:
            new_name = f"{name}_{target_func}"
            new_token_map[node_id] = (new_name, file, line)
        else:
            new_token_map[node_id] = (name, file, line)
            
    return new_token_map

def build_index_to_id_map(token_map, func_name_map):
    # creates index to id map and demangles function names in pointees
    for idx in token_map.keys():
        if idx in func_name_map:
            token_map[idx] = (func_name_map[idx], token_map[idx][1], token_map[idx][2])
    return {idx: token_map[idx][0] for idx in token_map.keys()}

def display_pts_to_info(graph, id_map):
    print(f"{'Pointer':<20} {'Pointees'}")
    print("-" * 30)
    # sort for deterministic output
    for pointer in sorted(graph):
        if pointer not in id_map:
            continue
        pointee_ids = sorted(
            id_map[p] for p in graph[pointer] if p in id_map
        )
        print(f"{id_map[pointer]:<20} {', '.join(pointee_ids)}")

def remove_stl_pointers(graph, token_map):
    for pointer in list(graph.keys()):
        if pointer not in token_map:
            graph.pop(pointer)
            continue
        if "std::" in token_map[pointer][0]:
            graph.pop(pointer)
            continue
        for pointee in list(graph[pointer]):
            if "std::" in token_map[pointee][0]:
                graph[pointer].remove(pointee)

# def filter_graph(graph, token_map):
#     # Filter out noise variables:
#     # - LLVM temporaries (call12 etc)
#     # - Compiler generated (.addr, __ etc)
#     # - STL internals
    
#     nodes_to_remove = set()
    
#     # Regex for "call" followed by digits (e.g. call12) or dot (call.i)
#     call_pattern = re.compile(r"^call(\d+|\.)")
    
#     for node, (name, file, line) in token_map.items():            
#         # Note: name might be suffixed with _main, so check start
#         if name.startswith("call"):
#             # Check if it is strictly call + digits/dot + optional suffix
#             # e.g. call12_main or call_main or call.i
#             parts = name.split('_')
#             base = parts[0]
#             if base == "call" or call_pattern.match(base):
#                 nodes_to_remove.add(node)
#                 continue
        
#         # 3. Compiler internals (starts with __ or .)
#         if name.startswith("__") or name.startswith("."):
#             nodes_to_remove.add(node)
#             continue
            
#         # 4. Address temporaries
#         if ".addr" in name:
#             nodes_to_remove.add(node)
#             continue

#         # 5. Inlined internals (often end in .i or .i123)
#         # But be careful not to kill user variables that happened to be inlined?
#         # User variables usually don't get renamed with .i unless there is a collision or they are local statics?
#         # In STL case, __first.addr.i is already caught by __.
#         # call.i is caught by call.
#         # Let's check test_output.txt: `v_main` is fine. `call.i` is fine.
#         # So maybe just the above rules are enough.
            
#     for node in nodes_to_remove:
#         if node in graph:
#             graph.pop(node)
#         if node in token_map:
#             token_map.pop(node)
            
#     # Also clean up graph edges pointing to removed nodes?
#     # No, remap_nodes will handle keys not in token_map?
#     # Wait, remap_nodes iterates graph.items(). If we removed from graph, we are good.
#     # But if A points to RemovedNode, A remains in graph. 
#     # remap_nodes: `mapped_points = {node_mapping[p] for p in points if p in node_mapping}`
#     # Since we removed RemovedNode from token_map, it won't be in node_mapping (built from token_map keys).
#     # So it will be effectively filtered from edges too.
    
#     return graph, token_map

if __name__ == "__main__":

    parser = argparse.ArgumentParser(description="A utility to parse SVF output and print source variables' points-to information on the terminal.")
    parser.add_argument("filename", help="Input file name")
    args = parser.parse_args()
    filename = args.filename

    pta_file = filename + ".pta"
    if not Path(pta_file).exists():
        print(f"Error: {pta_file} not found")
        exit(1)

    pta_graph, token_map = parse_pta_input(pta_file)
    
    dot_file = "callgraph_final.dot" # Parse callgraph to get function names
    func_name_map = parse_callgraph_dot(dot_file) # func_name_map just maps mangled function names to demangled names. The purpose of this function is just to identify the functions present in the program   
    token_map = map_variables_to_functions(token_map, func_name_map)
    remove_dead_nodes(pta_graph, token_map)
    remove_stl_pointers(pta_graph, token_map) # seems a bit scammy, check this out carefully
    # pta_graph, token_map = filter_graph(pta_graph, token_map) # Filter noise
    pta_graph, token_map = remap_nodes(pta_graph, token_map)
    index_to_id_map = build_index_to_id_map(token_map, func_name_map)
    display_pts_to_info(pta_graph, index_to_id_map)