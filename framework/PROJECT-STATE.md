# Native project state

`Cell::StateTree` owns the state model, XML reader/writer and binary attribute codec. It does not wrap a third-party state tree or XML DOM. Link `Cell::StateTree` and include `cell/state_tree.hpp` or `cell/xml.hpp`.

`Tree` handles share a mutable control-thread model. `createCopy()` explicitly copies nodes and properties; immutable binary payloads remain shared. Signed and unsigned 64-bit scalars retain their exact identity independently of doubles. Checked numeric narrowing rejects values outside the requested representation. Callers must catch failures at public read/restore boundaries. These APIs are not audio-callback services.

The XML API supports project attributes and nested elements. It rejects non-whitespace text content, DTDs, custom entities, invalid UTF-8 and duplicate decoded properties. It is not a mixed-content document editor. Element names and attribute spelling remain compatible with existing project state, including `base64:` binary property names and the established length-prefixed six-bit payload encoding. That payload encoding is not RFC 4648 Base64.

Default project parsing derives admission budgets from the actual input size rather than imposing a track, event, duration or nesting ceiling. Explicit `legacy::Limits` remain available when a caller needs a smaller import budget. Parsing, cloning, writing and teardown use iterative traversals. File output uses atomic replacement. Memory and filesystem representation still constrain achievable project sizes; removing policy caps does not guarantee a machine can load every input.

`xml::encodeBinary` and `decodeBinary` preserve the established host XML envelope. Its unsigned 32-bit length is a wire-format constraint. Direct project XML files do not inherit that envelope limit.

Linux SDK contracts exercise 300,000 state entries, 5,000 nesting levels, exact binary payloads, numeric overflow/underflow and large identities. Product compatibility and host qualification are separate checks. Windows host, input, DPI and teardown qualification remain required. No Mac compilation is part of this migration checkpoint.
