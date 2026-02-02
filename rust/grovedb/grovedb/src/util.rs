pub(crate) mod compat;

use grovedb_storage::Storage;

use crate::{Error, RocksDbStorage, Transaction, TransactionArg};

pub(crate) enum TxRef<'a, 'db: 'a> {
    Owned(Transaction<'db>),
    Borrowed(&'a Transaction<'db>),
}

impl<'a, 'db> TxRef<'a, 'db> {
    pub(crate) fn new(db: &'db RocksDbStorage, transaction_arg: TransactionArg<'db, 'a>) -> Self {
        if let Some(tx) = transaction_arg {
            Self::Borrowed(tx)
        } else {
            Self::Owned(db.start_transaction())
        }
    }

    /// Commit the transaction if it wasn't received from outside
    pub(crate) fn commit_local(self) -> Result<(), Error> {
        match self {
            TxRef::Owned(tx) => tx
                .commit()
                .map_err(|e| grovedb_storage::Error::from(e).into()),
            TxRef::Borrowed(_) => Ok(()),
        }
    }
}

impl<'db> AsRef<Transaction<'db>> for TxRef<'_, 'db> {
    fn as_ref(&self) -> &Transaction<'db> {
        match self {
            TxRef::Owned(tx) => tx,
            TxRef::Borrowed(tx) => tx,
        }
    }
}
