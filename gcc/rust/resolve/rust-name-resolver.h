// Copyright (C) 2020 Free Software Foundation, Inc.

// This file is part of GCC.

// GCC is free software; you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 3, or (at your option) any later
// version.

// GCC is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
// for more details.

// You should have received a copy of the GNU General Public License
// along with GCC; see the file COPYING3.  If not see
// <http://www.gnu.org/licenses/>.

#ifndef RUST_NAME_RESOLVER_H
#define RUST_NAME_RESOLVER_H

#include "rust-system.h"
#include "rust-hir-map.h"
#include "rust-hir-type-check.h"

namespace Rust {
namespace Resolver {

class Rib
{
public:
  // Rusts uses local_def_ids assigned by def_collector on the AST
  // lets use NodeId instead
  Rib (CrateNum crateNum, NodeId node_id)
    : crate_num (crateNum), node_id (node_id)
  {}

  ~Rib () {}

  void insert_name (std::string ident, NodeId id, Location locus)
  {
    mappings[ident] = id;
    decls_within_rib.insert (std::pair<NodeId, Location> (id, locus));
    references[id] = {};
  }

  bool lookup_name (std::string ident, NodeId *id)
  {
    auto it = mappings.find (ident);
    if (it == mappings.end ())
      return false;

    *id = it->second;
    return true;
  }

  CrateNum get_crate_num () const { return crate_num; }
  NodeId get_node_id () const { return node_id; }

  void iterate_decls (std::function<bool (NodeId, Location)> cb)
  {
    for (auto it : decls_within_rib)
      {
	if (!cb (it.first, it.second))
	  return;
      }
  }

  void iterate_references_for_def (NodeId def, std::function<bool (NodeId)> cb)
  {
    auto it = references.find (def);
    if (it == references.end ())
      return;

    for (auto ref : it->second)
      {
	if (!cb (ref))
	  return;
      }
  }

  void append_reference_for_def (NodeId def, NodeId ref)
  {
    references[def].insert (ref);
  }

  bool have_references_for_node (NodeId def) const
  {
    auto it = references.find (def);
    if (it == references.end ())
      return false;

    return !it->second.empty ();
  }

  bool decl_was_declared_here (NodeId def) const
  {
    for (auto &it : decls_within_rib)
      {
	if (it.first == def)
	  return true;
      }
    return false;
  }

private:
  CrateNum crate_num;
  NodeId node_id;
  std::map<std::string, NodeId> mappings;
  std::set<std::pair<NodeId, Location> > decls_within_rib;
  std::map<NodeId, std::set<NodeId> > references;
};

class Scope
{
public:
  Scope (CrateNum crate_num) : crate_num (crate_num) {}
  ~Scope () {}

  void insert (std::string ident, NodeId id, Location locus)
  {
    peek ()->insert_name (ident, id, locus);
  }

  bool lookup (std::string ident, NodeId *id)
  {
    NodeId lookup = UNKNOWN_NODEID;
    iterate ([&] (Rib *r) mutable -> bool {
      if (r->lookup_name (ident, &lookup))
	return false;
      return true;
    });

    *id = lookup;
    return lookup != UNKNOWN_NODEID;
  }

  void iterate (std::function<bool (Rib *)> cb)
  {
    for (auto it = stack.rbegin (); it != stack.rend (); ++it)
      {
	if (!cb (*it))
	  return;
      }
  }

  Rib *peek () { return stack.back (); }

  void push (NodeId id) { stack.push_back (new Rib (get_crate_num (), id)); }

  Rib *pop ()
  {
    Rib *r = peek ();
    stack.pop_back ();
    return r;
  }

  CrateNum get_crate_num () const { return crate_num; }

  void append_reference_for_def (NodeId refId, NodeId defId)
  {
    bool ok = false;
    iterate ([&] (Rib *r) mutable -> bool {
      if (r->decl_was_declared_here (defId))
	{
	  ok = true;
	  r->append_reference_for_def (defId, refId);
	  return false;
	}
      return true;
    });
    rust_assert (ok);
  }

private:
  CrateNum crate_num;
  std::vector<Rib *> stack;
};

// This can map simple NodeIds for names to their parent node
// for example:
//
// var x = y + 1;
//
// say y has node id=1 and the plus_expression has id=2
// then the Definition will have
// Definition { node=1, parent=2 }
// this will be used later to gather the ribs for the type inferences context
//
// if parent is UNKNOWN_NODEID then this is a root declaration
// say the var_decl hasa node_id=4;
// the parent could be a BLOCK_Expr node_id but lets make it UNKNOWN_NODE_ID
// so we know when it terminates
struct Definition
{
  NodeId node;
  NodeId parent;
  // add kind ?
};

class Resolver
{
public:
  static Resolver *get ();
  ~Resolver () {}

  // these builtin types
  void insert_builtin_types (Rib *r);

  // these will be required for type resolution passes to
  // map back to tyty nodes
  std::vector<AST::TypePath *> &get_builtin_types ();

  void push_new_name_rib (Rib *r);
  void push_new_type_rib (Rib *r);

  bool find_name_rib (NodeId id, Rib **rib);
  bool find_type_rib (NodeId id, Rib **rib);

  void insert_new_definition (NodeId id, Definition def);
  bool lookup_definition (NodeId id, Definition *def);

  void insert_resolved_name (NodeId refId, NodeId defId);
  bool lookup_resolved_name (NodeId refId, NodeId *defId);

  void insert_resolved_type (NodeId refId, NodeId defId);
  bool lookup_resolved_type (NodeId refId, NodeId *defId);

  // proxy for scoping
  Scope &get_name_scope () { return name_scope; }
  Scope &get_type_scope () { return type_scope; }

  NodeId get_global_type_node_id () { return global_type_node_id; }

private:
  Resolver ();

  void generate_builtins ();

  Analysis::Mappings *mappings;
  TypeCheckContext *tyctx;

  std::vector<AST::TypePath *> builtins;

  Scope name_scope;
  Scope type_scope;

  NodeId global_type_node_id;

  // map a AST Node to a Rib
  std::map<NodeId, Rib *> name_ribs;
  std::map<NodeId, Rib *> type_ribs;

  // map any Node to its Definition
  // ie any name or type usage
  std::map<NodeId, Definition> name_definitions;

  // Rust uses DefIds to namespace these under a crate_num
  // but then it uses the def_collector to assign local_defids
  // to each ast node as well. not sure if this is going to fit
  // with gcc very well to compile a full crate in one go but we will
  // see.

  // these are of the form ref->Def-NodeId
  // we need two namespaces one for names and ones for types
  std::map<NodeId, NodeId> resolved_names;
  std::map<NodeId, NodeId> resolved_types;
};

} // namespace Resolver
} // namespace Rust

#endif // RUST_NAME_RESOLVER_H
