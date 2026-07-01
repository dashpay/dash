//! Visualize

use std::io::{Result, Write};

use bincode::{
    config,
    config::{BigEndian, Configuration},
};
use grovedb_merk::{Merk, VisualizeableMerk};
use grovedb_path::SubtreePathBuilder;
use grovedb_storage::{Storage, StorageContext};
use grovedb_version::version::GroveVersion;
use grovedb_visualize::{visualize_stdout, Drawer, Visualize};

use crate::{
    element::{elements_iterator::ElementIteratorExtensions, Element},
    util::TxRef,
    GroveDb, TransactionArg,
};

impl GroveDb {
    fn draw_subtree<W: Write, B: AsRef<[u8]>>(
        &self,
        mut drawer: Drawer<W>,
        path: SubtreePathBuilder<'_, B>,
        transaction: TransactionArg,
        grove_version: &GroveVersion,
    ) -> Result<Drawer<W>> {
        drawer.down();

        let tx = TxRef::new(&self.db, transaction);

        let storage = self
            .db
            .get_transactional_storage_context((&path).into(), None, tx.as_ref())
            .unwrap();

        let mut iter = Element::iterator(storage.raw_iter()).unwrap();
        while let Some((key, element)) = iter
            .next_element(grove_version)
            .unwrap()
            .expect("cannot get next element")
        {
            drawer.write(b"\n[key: ")?;
            drawer = key.visualize(drawer)?;
            drawer.write(b" ")?;
            match element {
                Element::Tree(..) => {
                    drawer.write(b"Merk root is: ")?;
                    drawer = element.visualize(drawer)?;
                    drawer.down();
                    drawer = self.draw_subtree(
                        drawer,
                        path.derive_owned_with_child(key),
                        transaction,
                        grove_version,
                    )?;
                    drawer.up();
                }
                other => {
                    drawer = other.visualize(drawer)?;
                }
            }
        }

        drawer.up();
        Ok(drawer)
    }

    fn draw_root_tree<W: Write>(
        &self,
        mut drawer: Drawer<W>,
        transaction: TransactionArg,
        grove_version: &GroveVersion,
    ) -> Result<Drawer<W>> {
        drawer.down();

        drawer = self.draw_subtree(
            drawer,
            SubtreePathBuilder::new(),
            transaction,
            grove_version,
        )?;

        drawer.up();
        Ok(drawer)
    }

    fn visualize_start<W: Write>(
        &self,
        mut drawer: Drawer<W>,
        transaction: TransactionArg,
        grove_version: &GroveVersion,
    ) -> Result<Drawer<W>> {
        drawer.write(b"root")?;
        drawer = self.draw_root_tree(drawer, transaction, grove_version)?;
        drawer.flush()?;
        Ok(drawer)
    }
}

impl Visualize for GroveDb {
    fn visualize<W: Write>(&self, drawer: Drawer<W>) -> Result<Drawer<W>> {
        self.visualize_start(drawer, None, GroveVersion::latest())
    }
}

#[allow(dead_code)]
pub fn visualize_merk_stdout<'db, S: StorageContext<'db>>(merk: &Merk<S>) {
    visualize_stdout(&VisualizeableMerk::new(merk, |bytes: &[u8]| {
        let config = config::standard().with_big_endian().with_no_limit();
        bincode::decode_from_slice::<Element, Configuration<BigEndian>>(bytes, config)
            .expect("unable to deserialize Element")
            .0
    }));
}
