//! GroveDB binding for Node.JS

#![deny(missing_docs)]

mod converter;

use std::{option::Option::None, path::Path, sync::mpsc, thread};

use grovedb::{GroveDb, Transaction, TransactionArg};
use grovedb_version::version::GroveVersion;
use neon::prelude::*;

type DbCallback = Box<dyn for<'a> FnOnce(&'a GroveDb, TransactionArg, &Channel) + Send>;
type UnitCallback = Box<dyn FnOnce(&Channel) + Send>;

// Messages sent on the database channel
enum DbMessage {
    // Callback to be executed
    Callback(DbCallback),
    // Indicates that the thread should be stopped and connection closed
    Close(UnitCallback),
    StartTransaction(UnitCallback),
    CommitTransaction(UnitCallback),
    RollbackTransaction(UnitCallback),
    AbortTransaction(UnitCallback),
    Flush(UnitCallback),
}

struct GroveDbWrapper {
    tx: mpsc::Sender<DbMessage>,
}

// Internal wrapper logic. Needed to avoid issues with passing threads to
// node.js. Avoiding thread conflicts by having a dedicated thread for the
// groveDB instance and uses events to communicate with it
impl GroveDbWrapper {
    // Creates a new instance of `GroveDbWrapper`
    //
    // 1. Creates a connection and a channel
    // 2. Spawns a thread and moves the channel receiver and connection to it
    // 3. On a separate thread, read closures off the channel and execute with
    // access    to the connection.
    fn new(cx: &mut FunctionContext) -> NeonResult<Self> {
        let path_string = cx.argument::<JsString>(0)?.value(cx);

        // Channel for sending callbacks to execute on the GroveDb connection thread
        let (tx, rx) = mpsc::channel::<DbMessage>();

        // Create an `Channel` for calling back to JavaScript. It is more efficient
        // to create a single channel and re-use it for all database callbacks.
        // The JavaScript process will not exit as long as this channel has not been
        // dropped.
        let channel = cx.channel();

        // Spawn a thread for processing database queries
        // This will not block the JavaScript main thread and will continue executing
        // concurrently.
        thread::spawn(move || {
            let path = Path::new(&path_string);
            // Open a connection to groveDb, this will be moved to a separate thread
            // TODO: think how to pass this error to JS
            let grove_db = GroveDb::open(path).unwrap();

            let mut transaction: Option<Transaction> = None;

            // Blocks until a callback is available
            // When the instance of `Database` is dropped, the channel will be closed
            // and `rx.recv()` will return an `Err`, ending the loop and terminating
            // the thread.
            while let Ok(message) = rx.recv() {
                match message {
                    DbMessage::Callback(callback) => {
                        // The connection and channel are owned by the thread, but _lent_ to
                        // the callback. The callback has exclusive access to the connection
                        // for the duration of the callback.
                        callback(&grove_db, transaction.as_ref(), &channel);
                    }
                    // Immediately close the connection, even if there are pending messages
                    DbMessage::Close(callback) => {
                        drop(transaction);
                        drop(grove_db);
                        callback(&channel);
                        break;
                    }
                    // Flush message
                    DbMessage::Flush(callback) => {
                        grove_db.flush().unwrap();

                        callback(&channel);
                    }
                    DbMessage::StartTransaction(callback) => {
                        transaction = Some(grove_db.start_transaction());
                        callback(&channel);
                    }
                    DbMessage::CommitTransaction(callback) => {
                        grove_db
                            .commit_transaction(transaction.take().unwrap())
                            .unwrap()
                            .unwrap();
                        callback(&channel);
                    }
                    DbMessage::RollbackTransaction(callback) => {
                        grove_db
                            .rollback_transaction(&transaction.take().unwrap())
                            .unwrap();
                        callback(&channel);
                    }
                    DbMessage::AbortTransaction(callback) => {
                        drop(transaction.take().unwrap());
                        callback(&channel);
                    }
                }
            }
        });

        Ok(Self { tx })
    }

    // Idiomatic rust would take an owned `self` to prevent use after close
    // However, it's not possible to prevent JavaScript from continuing to hold a
    // closed database
    fn close(
        &self,
        callback: impl FnOnce(&Channel) + Send + 'static,
    ) -> Result<(), mpsc::SendError<DbMessage>> {
        self.tx.send(DbMessage::Close(Box::new(callback)))
    }

    // Idiomatic rust would take an owned `self` to prevent use after close
    // However, it's not possible to prevent JavaScript from continuing to hold a
    // closed database
    fn flush(
        &self,
        callback: impl FnOnce(&Channel) + Send + 'static,
    ) -> Result<(), mpsc::SendError<DbMessage>> {
        self.tx.send(DbMessage::Flush(Box::new(callback)))
    }

    fn send_to_db_thread(
        &self,
        callback: impl for<'a> FnOnce(&'a GroveDb, TransactionArg, &Channel) + Send + 'static,
    ) -> Result<(), mpsc::SendError<DbMessage>> {
        self.tx.send(DbMessage::Callback(Box::new(callback)))
    }

    fn start_transaction(
        &self,
        callback: impl FnOnce(&Channel) + Send + 'static,
    ) -> Result<(), mpsc::SendError<DbMessage>> {
        self.tx
            .send(DbMessage::StartTransaction(Box::new(callback)))
    }

    fn commit_transaction(
        &self,
        callback: impl FnOnce(&Channel) + Send + 'static,
    ) -> Result<(), mpsc::SendError<DbMessage>> {
        self.tx
            .send(DbMessage::CommitTransaction(Box::new(callback)))
    }

    fn rollback_transaction(
        &self,
        callback: impl FnOnce(&Channel) + Send + 'static,
    ) -> Result<(), mpsc::SendError<DbMessage>> {
        self.tx
            .send(DbMessage::RollbackTransaction(Box::new(callback)))
    }

    fn abort_transaction(
        &self,
        callback: impl FnOnce(&Channel) + Send + 'static,
    ) -> Result<(), mpsc::SendError<DbMessage>> {
        self.tx
            .send(DbMessage::AbortTransaction(Box::new(callback)))
    }
}

// Ensures that GroveDbWrapper is properly disposed when the corresponding JS
// object gets garbage collected
impl Finalize for GroveDbWrapper {}

// External wrapper logic
impl GroveDbWrapper {
    // Create a new instance of `Database` and place it inside a `JsBox`
    // JavaScript can hold a reference to a `JsBox`, but the contents are opaque
    fn js_open(mut cx: FunctionContext) -> JsResult<JsBox<Self>> {
        let grove_db_wrapper = Self::new(&mut cx).or_else(|err| cx.throw_error(err.to_string()))?;

        Ok(cx.boxed(grove_db_wrapper))
    }

    fn js_start_transaction(mut cx: FunctionContext) -> JsResult<JsUndefined> {
        let js_callback = cx.argument::<JsFunction>(0)?.root(&mut cx);

        let db = cx.this().downcast_or_throw::<JsBox<Self>, _>(&mut cx)?;

        db.start_transaction(|channel| {
            channel.send(move |mut task_context| {
                let callback = js_callback.into_inner(&mut task_context);
                let this = task_context.undefined();
                let callback_arguments: Vec<Handle<JsValue>> = vec![task_context.null().upcast()];

                callback.call(&mut task_context, this, callback_arguments)?;

                Ok(())
            });
        })
        .or_else(|err| cx.throw_error(err.to_string()))?;

        Ok(cx.undefined())
    }

    fn js_commit_transaction(mut cx: FunctionContext) -> JsResult<JsUndefined> {
        let js_callback = cx.argument::<JsFunction>(0)?.root(&mut cx);

        let db = cx.this().downcast_or_throw::<JsBox<Self>, _>(&mut cx)?;

        db.commit_transaction(|channel| {
            channel.send(move |mut task_context| {
                let callback = js_callback.into_inner(&mut task_context);
                let this = task_context.undefined();
                let callback_arguments: Vec<Handle<JsValue>> = vec![task_context.null().upcast()];

                callback.call(&mut task_context, this, callback_arguments)?;

                Ok(())
            });
        })
        .or_else(|err| cx.throw_error(err.to_string()))?;

        Ok(cx.undefined())
    }

    fn js_rollback_transaction(mut cx: FunctionContext) -> JsResult<JsUndefined> {
        let js_callback = cx.argument::<JsFunction>(0)?.root(&mut cx);

        let db = cx.this().downcast_or_throw::<JsBox<Self>, _>(&mut cx)?;

        db.rollback_transaction(|channel| {
            channel.send(move |mut task_context| {
                let callback = js_callback.into_inner(&mut task_context);
                let this = task_context.undefined();
                let callback_arguments: Vec<Handle<JsValue>> = vec![task_context.null().upcast()];

                callback.call(&mut task_context, this, callback_arguments)?;

                Ok(())
            });
        })
        .or_else(|err| cx.throw_error(err.to_string()))?;

        Ok(cx.undefined())
    }

    fn js_is_transaction_started(mut cx: FunctionContext) -> JsResult<JsUndefined> {
        let js_callback = cx.argument::<JsFunction>(0)?.root(&mut cx);

        let db = cx.this().downcast_or_throw::<JsBox<Self>, _>(&mut cx)?;

        db.send_to_db_thread(move |_grove_db: &GroveDb, transaction, channel| {
            let result = transaction.is_some();

            channel.send(move |mut task_context| {
                let callback = js_callback.into_inner(&mut task_context);
                let this = task_context.undefined();

                // First parameter of JS callbacks is error, which is null in this case
                let callback_arguments: Vec<Handle<JsValue>> = vec![
                    task_context.null().upcast(),
                    task_context.boolean(result).upcast(),
                ];

                callback.call(&mut task_context, this, callback_arguments)?;

                Ok(())
            });
        })
        .or_else(|err| cx.throw_error(err.to_string()))?;

        Ok(cx.undefined())
    }

    fn js_abort_transaction(mut cx: FunctionContext) -> JsResult<JsUndefined> {
        let js_callback = cx.argument::<JsFunction>(0)?.root(&mut cx);

        let db = cx.this().downcast_or_throw::<JsBox<Self>, _>(&mut cx)?;

        db.abort_transaction(|channel| {
            channel.send(move |mut task_context| {
                let callback = js_callback.into_inner(&mut task_context);
                let this = task_context.undefined();
                let callback_arguments: Vec<Handle<JsValue>> = vec![task_context.null().upcast()];

                callback.call(&mut task_context, this, callback_arguments)?;

                Ok(())
            });
        })
        .or_else(|err| cx.throw_error(err.to_string()))?;

        Ok(cx.undefined())
    }

    fn js_get(mut cx: FunctionContext) -> JsResult<JsUndefined> {
        let js_path = cx.argument::<JsArray>(0)?;
        let js_key = cx.argument::<JsBuffer>(1)?;
        let js_using_transaction = cx.argument::<JsBoolean>(2)?;
        let js_callback = cx.argument::<JsFunction>(3)?.root(&mut cx);

        let path = converter::js_array_of_buffers_to_vec(js_path, &mut cx)?;
        let key = converter::js_buffer_to_vec_u8(js_key, &mut cx);

        // Get the `this` value as a `JsBox<Database>`
        let db = cx.this().downcast_or_throw::<JsBox<Self>, _>(&mut cx)?;
        let using_transaction = js_using_transaction.value(&mut cx);

        db.send_to_db_thread(move |grove_db: &GroveDb, transaction, channel| {
            let result = grove_db
                .get(
                    path.as_slice(),
                    &key,
                    using_transaction.then_some(transaction).flatten(),
                    GroveVersion::latest(),
                )
                .unwrap(); // Todo: Costs

            channel.send(move |mut task_context| {
                let callback = js_callback.into_inner(&mut task_context);
                let this = task_context.undefined();
                let callback_arguments: Vec<Handle<JsValue>> = match result {
                    Ok(element) => {
                        // First parameter of JS callbacks is error, which is null in this case
                        vec![
                            task_context.null().upcast(),
                            converter::element_to_js_object(element, &mut task_context)?,
                        ]
                    }

                    // Convert the error to a JavaScript exception on failure
                    Err(err) => vec![task_context.error(err.to_string())?.upcast()],
                };

                callback.call(&mut task_context, this, callback_arguments)?;

                Ok(())
            });
        })
        .or_else(|err| cx.throw_error(err.to_string()))?;

        // The result is returned through the callback, not through direct return
        Ok(cx.undefined())
    }

    fn js_delete(mut cx: FunctionContext) -> JsResult<JsUndefined> {
        let js_path = cx.argument::<JsArray>(0)?;
        let js_key = cx.argument::<JsBuffer>(1)?;
        let js_using_transaction = cx.argument::<JsBoolean>(2)?;
        let js_callback = cx.argument::<JsFunction>(3)?.root(&mut cx);

        let path = converter::js_array_of_buffers_to_vec(js_path, &mut cx)?;
        let key = converter::js_buffer_to_vec_u8(js_key, &mut cx);

        let db = cx.this().downcast_or_throw::<JsBox<Self>, _>(&mut cx)?;
        let using_transaction = js_using_transaction.value(&mut cx);

        db.send_to_db_thread(move |grove_db: &GroveDb, transaction, channel| {
            let result = grove_db
                .delete(
                    path.as_slice(),
                    &key,
                    None,
                    using_transaction.then_some(transaction).flatten(),
                    GroveVersion::latest(),
                )
                .unwrap(); // Todo: Costs;

            channel.send(move |mut task_context| {
                let callback = js_callback.into_inner(&mut task_context);
                let this = task_context.undefined();
                let callback_arguments: Vec<Handle<JsValue>> = match result {
                    Ok(()) => {
                        vec![task_context.null().upcast()]
                    }

                    // Convert the error to a JavaScript exception on failure
                    Err(err) => vec![task_context.error(err.to_string())?.upcast()],
                };

                callback.call(&mut task_context, this, callback_arguments)?;

                Ok(())
            });
        })
        .or_else(|err| cx.throw_error(err.to_string()))?;

        // The result is returned through the callback, not through direct return
        Ok(cx.undefined())
    }

    fn js_insert(mut cx: FunctionContext) -> JsResult<JsUndefined> {
        let js_path = cx.argument::<JsArray>(0)?;
        let js_key = cx.argument::<JsBuffer>(1)?;
        let js_element = cx.argument::<JsObject>(2)?;
        let js_using_transaction = cx.argument::<JsBoolean>(3)?;
        let js_callback = cx.argument::<JsFunction>(4)?.root(&mut cx);

        let path = converter::js_array_of_buffers_to_vec(js_path, &mut cx)?;
        let key = converter::js_buffer_to_vec_u8(js_key, &mut cx);
        let element = converter::js_object_to_element(js_element, &mut cx)?;
        let using_transaction = js_using_transaction.value(&mut cx);

        // Get the `this` value as a `JsBox<Database>`
        let db = cx.this().downcast_or_throw::<JsBox<Self>, _>(&mut cx)?;

        db.send_to_db_thread(move |grove_db: &GroveDb, transaction, channel| {
            let result = grove_db
                .insert(
                    path.as_slice(),
                    &key,
                    element,
                    None,
                    using_transaction.then_some(transaction).flatten(),
                    GroveVersion::latest(),
                )
                .unwrap(); // Todo: Costs;

            channel.send(move |mut task_context| {
                let callback = js_callback.into_inner(&mut task_context);
                let this = task_context.undefined();
                let callback_arguments: Vec<Handle<JsValue>> = match result {
                    Ok(_) => vec![task_context.null().upcast()],
                    Err(err) => vec![task_context.error(err.to_string())?.upcast()],
                };

                callback.call(&mut task_context, this, callback_arguments)?;
                Ok(())
            });
        })
        .or_else(|err| cx.throw_error(err.to_string()))?;

        Ok(cx.undefined())
    }

    fn js_insert_if_not_exists(mut cx: FunctionContext) -> JsResult<JsUndefined> {
        let js_path = cx.argument::<JsArray>(0)?;
        let js_key = cx.argument::<JsBuffer>(1)?;
        let js_element = cx.argument::<JsObject>(2)?;
        let js_using_transaction = cx.argument::<JsBoolean>(3)?;
        let js_callback = cx.argument::<JsFunction>(4)?.root(&mut cx);

        let path = converter::js_array_of_buffers_to_vec(js_path, &mut cx)?;
        let key = converter::js_buffer_to_vec_u8(js_key, &mut cx);
        let element = converter::js_object_to_element(js_element, &mut cx)?;
        let using_transaction = js_using_transaction.value(&mut cx);

        // Get the `this` value as a `JsBox<Database>`
        let db = cx.this().downcast_or_throw::<JsBox<Self>, _>(&mut cx)?;

        db.send_to_db_thread(move |grove_db: &GroveDb, transaction, channel| {
            let result = grove_db
                .insert_if_not_exists(
                    path.as_slice(),
                    &key,
                    element,
                    using_transaction.then_some(transaction).flatten(),
                    GroveVersion::latest(),
                )
                .unwrap(); // Todo: Costs;

            channel.send(move |mut task_context| {
                let callback = js_callback.into_inner(&mut task_context);
                let this = task_context.undefined();
                let callback_arguments: Vec<Handle<JsValue>> = match result {
                    Ok(is_inserted) => vec![
                        task_context.null().upcast(),
                        task_context
                            .boolean(is_inserted)
                            .as_value(&mut task_context),
                    ],
                    Err(err) => vec![task_context.error(err.to_string())?.upcast()],
                };

                callback.call(&mut task_context, this, callback_arguments)?;
                Ok(())
            });
        })
        .or_else(|err| cx.throw_error(err.to_string()))?;

        Ok(cx.undefined())
    }

    fn js_put_aux(mut cx: FunctionContext) -> JsResult<JsUndefined> {
        let js_key = cx.argument::<JsBuffer>(0)?;
        let js_value = cx.argument::<JsBuffer>(1)?;
        let js_using_transaction = cx.argument::<JsBoolean>(2)?;
        let js_callback = cx.argument::<JsFunction>(3)?.root(&mut cx);

        let key = converter::js_buffer_to_vec_u8(js_key, &mut cx);
        let value = converter::js_buffer_to_vec_u8(js_value, &mut cx);

        let db = cx.this().downcast_or_throw::<JsBox<Self>, _>(&mut cx)?;
        let using_transaction = js_using_transaction.value(&mut cx);

        db.send_to_db_thread(move |grove_db: &GroveDb, transaction, channel| {
            let result = grove_db
                .put_aux(
                    &key,
                    &value,
                    None, // todo: support this
                    using_transaction.then_some(transaction).flatten(),
                )
                .unwrap(); // Todo: Costs;

            channel.send(move |mut task_context| {
                let callback = js_callback.into_inner(&mut task_context);
                let this = task_context.undefined();
                let callback_arguments: Vec<Handle<JsValue>> = match result {
                    Ok(()) => {
                        vec![task_context.null().upcast()]
                    }

                    // Convert the error to a JavaScript exception on failure
                    Err(err) => vec![task_context.error(err.to_string())?.upcast()],
                };

                callback.call(&mut task_context, this, callback_arguments)?;

                Ok(())
            });
        })
        .or_else(|err| cx.throw_error(err.to_string()))?;

        // The result is returned through the callback, not through direct return
        Ok(cx.undefined())
    }

    fn js_delete_aux(mut cx: FunctionContext) -> JsResult<JsUndefined> {
        let js_key = cx.argument::<JsBuffer>(0)?;
        let js_using_transaction = cx.argument::<JsBoolean>(1)?;
        let js_callback = cx.argument::<JsFunction>(2)?.root(&mut cx);

        let key = converter::js_buffer_to_vec_u8(js_key, &mut cx);

        let db = cx.this().downcast_or_throw::<JsBox<Self>, _>(&mut cx)?;
        let using_transaction = js_using_transaction.value(&mut cx);

        db.send_to_db_thread(move |grove_db: &GroveDb, transaction, channel| {
            let result = grove_db
                .delete_aux(
                    &key,
                    None,
                    using_transaction.then_some(transaction).flatten(),
                )
                .unwrap(); // Todo: Costs;

            channel.send(move |mut task_context| {
                let callback = js_callback.into_inner(&mut task_context);
                let this = task_context.undefined();
                let callback_arguments: Vec<Handle<JsValue>> = match result {
                    Ok(()) => {
                        vec![task_context.null().upcast()]
                    }

                    // Convert the error to a JavaScript exception on failure
                    Err(err) => vec![task_context.error(err.to_string())?.upcast()],
                };

                callback.call(&mut task_context, this, callback_arguments)?;

                Ok(())
            });
        })
        .or_else(|err| cx.throw_error(err.to_string()))?;

        // The result is returned through the callback, not through direct return
        Ok(cx.undefined())
    }

    fn js_get_aux(mut cx: FunctionContext) -> JsResult<JsUndefined> {
        let js_key = cx.argument::<JsBuffer>(0)?;
        let js_using_transaction = cx.argument::<JsBoolean>(1)?;
        let js_callback = cx.argument::<JsFunction>(2)?.root(&mut cx);

        let key = converter::js_buffer_to_vec_u8(js_key, &mut cx);

        let db = cx.this().downcast_or_throw::<JsBox<Self>, _>(&mut cx)?;
        let using_transaction = js_using_transaction.value(&mut cx);

        db.send_to_db_thread(move |grove_db: &GroveDb, transaction, channel| {
            let result = grove_db
                .get_aux(&key, using_transaction.then_some(transaction).flatten())
                .unwrap(); // Todo: Costs;

            channel.send(move |mut task_context| {
                let callback = js_callback.into_inner(&mut task_context);
                let this = task_context.undefined();
                let callback_arguments: Vec<Handle<JsValue>> = match result {
                    Ok(value) => {
                        if let Some(value) = value {
                            vec![
                                task_context.null().upcast(),
                                JsBuffer::external(&mut task_context, value).upcast(),
                            ]
                        } else {
                            vec![task_context.null().upcast(), task_context.null().upcast()]
                        }
                    }

                    // Convert the error to a JavaScript exception on failure
                    Err(err) => vec![task_context.error(err.to_string())?.upcast()],
                };

                callback.call(&mut task_context, this, callback_arguments)?;

                Ok(())
            });
        })
        .or_else(|err| cx.throw_error(err.to_string()))?;

        // The result is returned through the callback, not through direct return
        Ok(cx.undefined())
    }

    fn js_get_path_query(mut cx: FunctionContext) -> JsResult<JsUndefined> {
        let js_path_query = cx.argument::<JsObject>(0)?;
        let js_allows_cache = cx.argument::<JsBoolean>(1)?;
        let js_using_transaction = cx.argument::<JsBoolean>(2)?;

        let js_callback = cx.argument::<JsFunction>(3)?.root(&mut cx);

        let path_query = converter::js_path_query_to_path_query(js_path_query, &mut cx)?;

        let db = cx.this().downcast_or_throw::<JsBox<Self>, _>(&mut cx)?;
        let allows_cache = js_allows_cache.value(&mut cx);
        let using_transaction = js_using_transaction.value(&mut cx);

        db.send_to_db_thread(move |grove_db: &GroveDb, transaction, channel| {
            let result = grove_db
                .query_item_value(
                    &path_query,
                    allows_cache,
                    true,
                    true,
                    using_transaction.then_some(transaction).flatten(),
                    GroveVersion::latest(),
                )
                .unwrap(); // Todo: Costs;

            channel.send(move |mut task_context| {
                let callback = js_callback.into_inner(&mut task_context);
                let this = task_context.undefined();
                let callback_arguments: Vec<Handle<JsValue>> = match result {
                    Ok((value, skipped)) => {
                        let js_array: Handle<JsArray> = task_context.empty_array();
                        let js_vecs = converter::nested_vecs_to_js(value, &mut task_context)?;
                        let js_num = task_context.number(skipped).upcast::<JsValue>();
                        js_array.set(&mut task_context, 0, js_vecs)?;
                        js_array.set(&mut task_context, 1, js_num)?;
                        vec![task_context.null().upcast(), js_array.upcast()]
                    }

                    // Convert the error to a JavaScript exception on failure
                    Err(err) => vec![task_context.error(err.to_string())?.upcast()],
                };

                callback.call(&mut task_context, this, callback_arguments)?;

                Ok(())
            });
        })
        .or_else(|err| cx.throw_error(err.to_string()))?;

        // The result is returned through the callback, not through direct return
        Ok(cx.undefined())
    }

    /// Not implemented
    fn js_proof(mut cx: FunctionContext) -> JsResult<JsUndefined> {
        Ok(cx.undefined())
    }

    /// Sends a message to the DB thread to stop the thread and dispose the
    /// groveDb instance owned by it, then calls js callback passed as a first
    /// argument to the function
    fn js_close(mut cx: FunctionContext) -> JsResult<JsUndefined> {
        let js_callback = cx.argument::<JsFunction>(0)?.root(&mut cx);

        let db = cx.this().downcast_or_throw::<JsBox<Self>, _>(&mut cx)?;

        db.close(|channel| {
            channel.send(move |mut task_context| {
                let callback = js_callback.into_inner(&mut task_context);
                let this = task_context.undefined();
                let callback_arguments: Vec<Handle<JsValue>> = vec![task_context.null().upcast()];

                callback.call(&mut task_context, this, callback_arguments)?;

                Ok(())
            });
        })
        .or_else(|err| cx.throw_error(err.to_string()))?;

        Ok(cx.undefined())
    }

    /// Flush data on disc and then calls js callback passed as a first
    /// argument to the function
    fn js_flush(mut cx: FunctionContext) -> JsResult<JsUndefined> {
        let js_callback = cx.argument::<JsFunction>(0)?.root(&mut cx);

        let db = cx.this().downcast_or_throw::<JsBox<Self>, _>(&mut cx)?;

        db.flush(|channel| {
            channel.send(move |mut task_context| {
                let callback = js_callback.into_inner(&mut task_context);
                let this = task_context.undefined();
                let callback_arguments: Vec<Handle<JsValue>> = vec![task_context.null().upcast()];

                callback.call(&mut task_context, this, callback_arguments)?;

                Ok(())
            });
        })
        .or_else(|err| cx.throw_error(err.to_string()))?;

        Ok(cx.undefined())
    }

    /// Returns root hash or empty buffer
    fn js_root_hash(mut cx: FunctionContext) -> JsResult<JsUndefined> {
        let js_using_transaction = cx.argument::<JsBoolean>(0)?;
        let js_callback = cx.argument::<JsFunction>(1)?.root(&mut cx);

        let db = cx.this().downcast_or_throw::<JsBox<Self>, _>(&mut cx)?;

        let using_transaction = js_using_transaction.value(&mut cx);

        db.send_to_db_thread(move |grove_db: &GroveDb, transaction, channel| {
            let result = grove_db
                .root_hash(
                    using_transaction.then_some(transaction).flatten(),
                    GroveVersion::latest(),
                )
                .unwrap(); // Todo: Costs;

            channel.send(move |mut task_context| {
                let callback = js_callback.into_inner(&mut task_context);
                let this = task_context.undefined();

                let callback_arguments: Vec<Handle<JsValue>> = match result {
                    Ok(hash) => vec![
                        task_context.null().upcast(),
                        JsBuffer::external(&mut task_context, hash).upcast(),
                    ],
                    Err(err) => vec![task_context.error(err.to_string())?.upcast()],
                };

                callback.call(&mut task_context, this, callback_arguments)?;

                Ok(())
            });
        })
        .or_else(|err| cx.throw_error(err.to_string()))?;

        // The result is returned through the callback, not through direct return
        Ok(cx.undefined())
    }
}

#[neon::main]
fn main(mut cx: ModuleContext) -> NeonResult<()> {
    cx.export_function("groveDbOpen", GroveDbWrapper::js_open)?;
    cx.export_function("groveDbInsert", GroveDbWrapper::js_insert)?;
    cx.export_function(
        "groveDbInsertIfNotExists",
        GroveDbWrapper::js_insert_if_not_exists,
    )?;
    cx.export_function("groveDbGet", GroveDbWrapper::js_get)?;
    cx.export_function("groveDbDelete", GroveDbWrapper::js_delete)?;
    cx.export_function("groveDbProof", GroveDbWrapper::js_proof)?;
    cx.export_function("groveDbClose", GroveDbWrapper::js_close)?;
    cx.export_function("groveDbFlush", GroveDbWrapper::js_flush)?;
    cx.export_function(
        "groveDbStartTransaction",
        GroveDbWrapper::js_start_transaction,
    )?;
    cx.export_function(
        "groveDbCommitTransaction",
        GroveDbWrapper::js_commit_transaction,
    )?;
    cx.export_function(
        "groveDbRollbackTransaction",
        GroveDbWrapper::js_rollback_transaction,
    )?;
    cx.export_function(
        "groveDbIsTransactionStarted",
        GroveDbWrapper::js_is_transaction_started,
    )?;
    cx.export_function(
        "groveDbAbortTransaction",
        GroveDbWrapper::js_abort_transaction,
    )?;
    cx.export_function("groveDbPutAux", GroveDbWrapper::js_put_aux)?;
    cx.export_function("groveDbDeleteAux", GroveDbWrapper::js_delete_aux)?;
    cx.export_function("groveDbGetAux", GroveDbWrapper::js_get_aux)?;
    cx.export_function("groveDbGetPathQuery", GroveDbWrapper::js_get_path_query)?;
    cx.export_function("groveDbRootHash", GroveDbWrapper::js_root_hash)?;

    Ok(())
}
