## Versioning Protocol

### Versioning Elements

- Elements are persisted to state.
- In the future, we may make changes to the serialization method for elements or change the element structure itself.
- In such a case, backward compatibility with previously persisted state must be ensured.
- The current element enum looks like this:

```rust
pub enum Element {
    /// An ordinary value
    Item(Vec<u8>, Option<ElementFlags>),
    /// A reference to an object by its path
    Reference(ReferencePathType, MaxReferenceHop, Option<ElementFlags>),
    /// A subtree, contains the a prefixed key representing the root of the
    /// subtree.
    Tree(Option<Vec<u8>>, Option<ElementFlags>),
    /// Signed integer value that can be totaled in a sum tree
    SumItem(SumValue, Option<ElementFlags>),
    /// Same as Element::Tree but underlying Merk sums value of it's summable
    /// nodes
    SumTree(Option<Vec<u8>>, SumValue, Option<ElementFlags>),
}
```

- We might add a new variant or change the fields of an existing variant.
- If we do this, we need to ensure backward compatibility with previously persisted state by:
    - Creating a new `Element` structure with the updated changes.
    - Keeping the old structure (renamed based on version).
    - Implementing a deserializer for the old structure (bincode `Encode` and `Decode`).
    - Implementing a converter from the old structure to the new structure.
        - This will be possible because all updates are backward compatable.
- We can detect what element structure a piece of state refers to by checking the `TreeFeatureType` of the decoded Merk tree node.

**********************************Versioning Proofs**********************************

- At the start of every generated proof, we attach a varint encoded version number.