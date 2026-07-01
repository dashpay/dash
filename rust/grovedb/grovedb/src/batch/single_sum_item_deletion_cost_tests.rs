//! Tests

#[cfg(feature = "minimal")]
mod tests {
    use grovedb_merk::tree_type::TreeType;
    use grovedb_version::version::GroveVersion;

    use crate::{
        batch::QualifiedGroveDbOp,
        tests::{common::EMPTY_PATH, make_empty_grovedb},
        Element,
    };

    #[test]
    fn test_batch_one_deletion_sum_tree_costs_match_non_batch_on_transaction() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();

        let insertion_cost = db
            .insert(
                EMPTY_PATH,
                b"key1",
                Element::empty_sum_tree(),
                None,
                None,
                grove_version,
            )
            .cost_as_result()
            .expect("expected to insert successfully");

        let tx = db.start_transaction();

        let non_batch_cost = db
            .delete(EMPTY_PATH, b"key1", None, Some(&tx), grove_version)
            .cost_as_result()
            .expect("expected to delete successfully");

        assert_eq!(
            insertion_cost.storage_cost.added_bytes,
            non_batch_cost
                .storage_cost
                .removed_bytes
                .total_removed_bytes()
        );

        tx.rollback().expect("expected to rollback");
        let ops = vec![QualifiedGroveDbOp::delete_tree_op(
            vec![],
            b"key1".to_vec(),
            TreeType::NormalTree,
        )];
        let batch_cost = db
            .apply_batch(ops, None, Some(&tx), grove_version)
            .cost_as_result()
            .expect("expected to delete successfully");
        assert_eq!(non_batch_cost.storage_cost, batch_cost.storage_cost);
    }

    #[test]
    fn test_batch_one_deletion_sum_item_costs_match_non_batch_on_transaction() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();

        db.insert(
            EMPTY_PATH,
            b"sum_tree".as_slice(),
            Element::empty_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert sum tree");

        let insertion_cost = db
            .insert(
                [b"sum_tree".as_slice()].as_ref(),
                b"key1",
                Element::new_sum_item(15),
                None,
                None,
                grove_version,
            )
            .cost_as_result()
            .expect("expected to insert successfully");

        let tx = db.start_transaction();

        let non_batch_cost = db
            .delete(
                [b"sum_tree".as_slice()].as_ref(),
                b"key1",
                None,
                Some(&tx),
                grove_version,
            )
            .cost_as_result()
            .expect("expected to delete successfully");

        assert_eq!(
            insertion_cost.storage_cost.added_bytes,
            non_batch_cost
                .storage_cost
                .removed_bytes
                .total_removed_bytes()
        );

        tx.rollback().expect("expected to rollback");
        let ops = vec![QualifiedGroveDbOp::delete_op(
            vec![b"sum_tree".to_vec()],
            b"key1".to_vec(),
        )];
        let batch_cost = db
            .apply_batch(ops, None, Some(&tx), grove_version)
            .cost_as_result()
            .expect("expected to delete successfully");
        assert_eq!(non_batch_cost.storage_cost, batch_cost.storage_cost);
    }

    #[test]
    fn test_batch_one_deletion_sum_tree_with_flags_costs_match_non_batch_on_transaction() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();

        let insertion_cost = db
            .insert(
                EMPTY_PATH,
                b"key1",
                Element::empty_sum_tree_with_flags(Some(b"dog".to_vec())),
                None,
                None,
                grove_version,
            )
            .cost_as_result()
            .expect("expected to insert successfully");

        let tx = db.start_transaction();

        let non_batch_cost = db
            .delete(EMPTY_PATH, b"key1", None, Some(&tx), grove_version)
            .cost_as_result()
            .expect("expected to delete successfully");

        assert_eq!(insertion_cost.storage_cost.added_bytes, 128);
        assert_eq!(
            insertion_cost.storage_cost.added_bytes,
            non_batch_cost
                .storage_cost
                .removed_bytes
                .total_removed_bytes()
        );

        tx.rollback().expect("expected to rollback");
        let ops = vec![QualifiedGroveDbOp::delete_tree_op(
            vec![],
            b"key1".to_vec(),
            TreeType::NormalTree,
        )];
        let batch_cost = db
            .apply_batch(ops, None, Some(&tx), grove_version)
            .cost_as_result()
            .expect("expected to delete successfully");
        assert_eq!(non_batch_cost.storage_cost, batch_cost.storage_cost);
    }
}
