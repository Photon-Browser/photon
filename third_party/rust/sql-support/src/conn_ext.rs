/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use rusqlite::{
    self,
    types::{FromSql, ToSql},
    Connection, Params, Result as SqlResult, Row, Savepoint, Transaction, TransactionBehavior,
};
use std::iter::FromIterator;
use std::ops::Deref;
use std::time::Instant;

use crate::maybe_cached::MaybeCached;
use crate::{debug, warn};

/// This trait exists so that we can use these helpers on `rusqlite::{Transaction, Connection}`.
/// Note that you must import ConnExt in order to call these methods on anything.
pub trait ConnExt {
    /// The method you need to implement to opt in to all of this.
    fn conn(&self) -> &Connection;

    /// Set the value of the pragma on the main database. Returns the same object, for chaining.
    fn set_pragma<T>(&self, pragma_name: &str, pragma_value: T) -> SqlResult<&Self>
    where
        T: ToSql,
        Self: Sized,
    {
        // None == Schema name, e.g. `PRAGMA some_attached_db.something = blah`
        self.conn()
            .pragma_update(None, pragma_name, &pragma_value)?;
        Ok(self)
    }

    /// Get a cached or uncached statement based on a flag.
    fn prepare_maybe_cached<'conn>(
        &'conn self,
        sql: &str,
        cache: bool,
    ) -> SqlResult<MaybeCached<'conn>> {
        MaybeCached::prepare(self.conn(), sql, cache)
    }

    /// Execute all the provided statements.
    fn execute_all(&self, stmts: &[&str]) -> SqlResult<()> {
        let conn = self.conn();
        for sql in stmts {
            let r = conn.execute(sql, []);
            match r {
                Ok(_) => {}
                // Ignore ExecuteReturnedResults error because they're pointless
                // and annoying.
                Err(rusqlite::Error::ExecuteReturnedResults) => {}
                Err(e) => return Err(e),
            }
        }
        Ok(())
    }

    /// Execute a single statement.
    fn execute_one(&self, stmt: &str) -> SqlResult<()> {
        self.execute_all(&[stmt])
    }

    /// Equivalent to `Connection::execute` but caches the statement so that subsequent
    /// calls to `execute_cached` will have improved performance.
    fn execute_cached<P: Params>(&self, sql: &str, params: P) -> SqlResult<usize> {
        let mut stmt = self.conn().prepare_cached(sql)?;
        stmt.execute(params)
    }

    /// Execute a query that returns a single result column, and return that result.
    fn query_one<T: FromSql>(&self, sql: &str) -> SqlResult<T> {
        let res: T = self.conn().query_row_and_then(sql, [], |row| row.get(0))?;
        Ok(res)
    }

    /// Return true if a query returns any rows
    fn exists<P: Params>(&self, sql: &str, params: P) -> SqlResult<bool> {
        let conn = self.conn();
        let mut stmt = conn.prepare(sql)?;
        let exists = stmt.query(params)?.next()?.is_some();
        Ok(exists)
    }

    /// Execute a query that returns 0 or 1 result columns, returning None
    /// if there were no rows, or if the only result was NULL.
    fn try_query_one<T: FromSql, P: Params>(
        &self,
        sql: &str,
        params: P,
        cache: bool,
    ) -> SqlResult<Option<T>>
    where
        Self: Sized,
    {
        use rusqlite::OptionalExtension;
        // The outer option is if we got rows, the inner option is
        // if the first row was null.
        let res: Option<Option<T>> = self
            .conn()
            .query_row_and_then_cachable(sql, params, |row| row.get(0), cache)
            .optional()?;
        // go from Option<Option<T>> to Option<T>
        Ok(res.unwrap_or_default())
    }

    /// Equivalent to `rusqlite::Connection::query_row_and_then` but allows
    /// passing a flag to indicate that it's cached.
    fn query_row_and_then_cachable<T, E, P, F>(
        &self,
        sql: &str,
        params: P,
        mapper: F,
        cache: bool,
    ) -> Result<T, E>
    where
        Self: Sized,
        P: Params,
        E: From<rusqlite::Error>,
        F: FnOnce(&Row<'_>) -> Result<T, E>,
    {
        Ok(self
            .try_query_row(sql, params, mapper, cache)?
            .ok_or(rusqlite::Error::QueryReturnedNoRows)?)
    }

    /// Helper for when you'd like to get a `Vec<T>` of all the rows returned by a
    /// query that takes named arguments. See also
    /// `query_rows_and_then_cached`.
    fn query_rows_and_then<T, E, P, F>(&self, sql: &str, params: P, mapper: F) -> Result<Vec<T>, E>
    where
        Self: Sized,
        P: Params,
        E: From<rusqlite::Error>,
        F: FnMut(&Row<'_>) -> Result<T, E>,
    {
        query_rows_and_then_cachable(self.conn(), sql, params, mapper, false)
    }

    /// Helper for when you'd like to get a `Vec<T>` of all the rows returned by a
    /// query that takes named arguments.
    fn query_rows_and_then_cached<T, E, P, F>(
        &self,
        sql: &str,
        params: P,
        mapper: F,
    ) -> Result<Vec<T>, E>
    where
        Self: Sized,
        P: Params,
        E: From<rusqlite::Error>,
        F: FnMut(&Row<'_>) -> Result<T, E>,
    {
        query_rows_and_then_cachable(self.conn(), sql, params, mapper, true)
    }

    /// Like `query_rows_and_then_cachable`, but works if you want a non-Vec as a result.
    /// # Example:
    /// ```rust,no_run
    /// # use std::collections::HashSet;
    /// # use sql_support::ConnExt;
    /// # use rusqlite::Connection;
    /// fn get_visit_tombstones(conn: &Connection, id: i64) -> rusqlite::Result<HashSet<i64>> {
    ///     Ok(conn.query_rows_into(
    ///         "SELECT visit_date FROM moz_historyvisit_tombstones
    ///          WHERE place_id = :place_id",
    ///         &[(":place_id", &id)],
    ///         |row| row.get::<_, i64>(0))?)
    /// }
    /// ```
    /// Note if the type isn't inferred, you'll have to do something gross like
    /// `conn.query_rows_into::<HashSet<_>, _, _, _>(...)`.
    fn query_rows_into<Coll, T, E, P, F>(&self, sql: &str, params: P, mapper: F) -> Result<Coll, E>
    where
        Self: Sized,
        E: From<rusqlite::Error>,
        F: FnMut(&Row<'_>) -> Result<T, E>,
        Coll: FromIterator<T>,
        P: Params,
    {
        query_rows_and_then_cachable(self.conn(), sql, params, mapper, false)
    }

    /// Same as `query_rows_into`, but caches the stmt if possible.
    fn query_rows_into_cached<Coll, T, E, P, F>(
        &self,
        sql: &str,
        params: P,
        mapper: F,
    ) -> Result<Coll, E>
    where
        Self: Sized,
        P: Params,
        E: From<rusqlite::Error>,
        F: FnMut(&Row<'_>) -> Result<T, E>,
        Coll: FromIterator<T>,
    {
        query_rows_and_then_cachable(self.conn(), sql, params, mapper, true)
    }

    // This should probably have a longer name...
    /// Like `query_row_and_then_cacheable` but returns None instead of erroring
    /// if no such row exists.
    fn try_query_row<T, E, P, F>(
        &self,
        sql: &str,
        params: P,
        mapper: F,
        cache: bool,
    ) -> Result<Option<T>, E>
    where
        Self: Sized,
        P: Params,
        E: From<rusqlite::Error>,
        F: FnOnce(&Row<'_>) -> Result<T, E>,
    {
        let conn = self.conn();
        let mut stmt = MaybeCached::prepare(conn, sql, cache)?;
        let mut rows = stmt.query(params)?;
        rows.next()?.map(mapper).transpose()
    }

    /// Caveat: This won't actually get used most of the time, and calls will
    /// usually invoke rusqlite's method with the same name. See comment on
    /// `UncheckedTransaction` for details (generally you probably don't need to
    /// care)
    fn unchecked_transaction(&self) -> SqlResult<UncheckedTransaction<'_>> {
        UncheckedTransaction::new(self.conn(), TransactionBehavior::Deferred)
    }

    /// Begin `unchecked_transaction` with `TransactionBehavior::Immediate`. Use
    /// when the first operation will be a read operation, that further writes
    /// depend on for correctness.
    fn unchecked_transaction_imm(&self) -> SqlResult<UncheckedTransaction<'_>> {
        UncheckedTransaction::new(self.conn(), TransactionBehavior::Immediate)
    }

    /// Get the DB size in bytes
    fn get_db_size(&self) -> Result<u32, rusqlite::Error> {
        let page_count: u32 = self.query_one("SELECT * from pragma_page_count()")?;
        let page_size: u32 = self.query_one("SELECT * from pragma_page_size()")?;
        let freelist_count: u32 = self.query_one("SELECT * from pragma_freelist_count()")?;

        Ok((page_count - freelist_count) * page_size)
    }
}

impl ConnExt for Connection {
    #[inline]
    fn conn(&self) -> &Connection {
        self
    }
}

impl ConnExt for Transaction<'_> {
    #[inline]
    fn conn(&self) -> &Connection {
        self
    }
}

impl ConnExt for Savepoint<'_> {
    #[inline]
    fn conn(&self) -> &Connection {
        self
    }
}

/// rusqlite, in an attempt to save us from ourselves, needs a mutable ref to a
/// connection to start a transaction. That is a bit of a PITA in some cases, so
/// we offer this as an alternative - but the responsibility of ensuring there
/// are no concurrent transactions is on our head.
///
/// This is very similar to the rusqlite `Transaction` - it doesn't prevent
/// against nested transactions but does allow you to use an immutable
/// `Connection`.
///
/// FIXME: This currently won't actually be used most of the time, because
/// `rusqlite` added [`Connection::unchecked_transaction`] (and
/// `Transaction::new_unchecked`, which can be used to reimplement
/// `unchecked_transaction_imm`), which will be preferred in a call to
/// `c.unchecked_transaction()`, because inherent methods have precedence over
/// methods on extension traits. The exception here is that this will still be
/// used by code which takes `&impl ConnExt` (I believe it would also be used if
/// you attempted to call `unchecked_transaction()` on a non-Connection that
/// implements ConnExt, such as a `Safepoint`, `UncheckedTransaction`, or
/// `Transaction` itself, but such code is clearly broken, so is not worth
/// considering).
///
/// The difference is that `rusqlite`'s version returns a normal
/// `rusqlite::Transaction`, rather than the `UncheckedTransaction` from this
/// crate. Aside from type's name and location (and the fact that `rusqlite`'s
/// detects slightly more misuse at compile time, and has more features), the
/// main difference is: `rusqlite`'s does not track when a transaction began,
/// which unfortunately seems to be used by the coop-transaction management in
/// places in some fashion.
///
/// There are at least two options for how to fix this:
/// 1. Decide we don't need this version, and delete it, and moving the
///    transaction timing into the coop-transaction code directly (or something
///    like this).
/// 2. Decide this difference *is* important, and rename
///    `ConnExt::unchecked_transaction` to something like
///    `ConnExt::transaction_unchecked`.
pub struct UncheckedTransaction<'conn> {
    pub conn: &'conn Connection,
    pub started_at: Instant,
    pub finished: bool,
    // we could add drop_behavior etc too, but we don't need it yet - we
    // always rollback.
}

impl<'conn> UncheckedTransaction<'conn> {
    /// Begin a new unchecked transaction. Cannot be nested, but this is not
    /// enforced by Rust (hence 'unchecked') - however, it is enforced by
    /// SQLite; use a rusqlite `savepoint` for nested transactions.
    pub fn new(conn: &'conn Connection, behavior: TransactionBehavior) -> SqlResult<Self> {
        let query = match behavior {
            TransactionBehavior::Deferred => "BEGIN DEFERRED",
            TransactionBehavior::Immediate => "BEGIN IMMEDIATE",
            TransactionBehavior::Exclusive => "BEGIN EXCLUSIVE",
            _ => unreachable!(),
        };
        conn.execute_batch(query)
            .map(move |_| UncheckedTransaction {
                conn,
                started_at: Instant::now(),
                finished: false,
            })
    }

    /// Consumes and commits an unchecked transaction.
    pub fn commit(mut self) -> SqlResult<()> {
        if self.finished {
            warn!("ignoring request to commit an already finished transaction");
            return Ok(());
        }
        self.finished = true;
        self.conn.execute_batch("COMMIT")?;
        debug!("Transaction commited after {:?}", self.started_at.elapsed());
        Ok(())
    }

    /// Consumes and rolls back an unchecked transaction.
    pub fn rollback(mut self) -> SqlResult<()> {
        if self.finished {
            warn!("ignoring request to rollback an already finished transaction");
            return Ok(());
        }
        self.rollback_()
    }

    fn rollback_(&mut self) -> SqlResult<()> {
        self.finished = true;
        self.conn.execute_batch("ROLLBACK")?;
        Ok(())
    }

    fn finish_(&mut self) -> SqlResult<()> {
        if self.finished || self.conn.is_autocommit() {
            return Ok(());
        }
        self.rollback_()?;
        Ok(())
    }
}

impl Deref for UncheckedTransaction<'_> {
    type Target = Connection;

    #[inline]
    fn deref(&self) -> &Connection {
        self.conn
    }
}

impl Drop for UncheckedTransaction<'_> {
    fn drop(&mut self) {
        if let Err(e) = self.finish_() {
            warn!("Error dropping an unchecked transaction: {}", e);
        }
    }
}

impl ConnExt for UncheckedTransaction<'_> {
    #[inline]
    fn conn(&self) -> &Connection {
        self
    }
}

fn query_rows_and_then_cachable<Coll, T, E, P, F>(
    conn: &Connection,
    sql: &str,
    params: P,
    mapper: F,
    cache: bool,
) -> Result<Coll, E>
where
    E: From<rusqlite::Error>,
    F: FnMut(&Row<'_>) -> Result<T, E>,
    Coll: FromIterator<T>,
    P: Params,
{
    let mut stmt = conn.prepare_maybe_cached(sql, cache)?;
    let iter = stmt.query_and_then(params, mapper)?;
    iter.collect::<Result<Coll, E>>()
}
