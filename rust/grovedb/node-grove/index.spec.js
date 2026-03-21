const fs = require('fs');

const { expect } = require('chai');

const GroveDB = require('./index');

const TEST_DATA_PATH = './test_data';

describe('GroveDB', () => {
  let groveDb;
  let treeKey;
  let itemKey;
  let itemValue;
  let rootTreePath;
  let itemTreePath;

  beforeEach(() => {
    groveDb = new GroveDB(TEST_DATA_PATH);

    treeKey = Buffer.from('test_tree');
    itemKey = Buffer.from('test_key');
    itemValue = Buffer.from('very nice test value');

    rootTreePath = [];
    itemTreePath = [treeKey];
  });

  afterEach(async () => {
    await groveDb.close();

    fs.rmSync(TEST_DATA_PATH, { recursive: true });
  });

  it('should store and retrieve a value', async () => {
    // Making a subtree to insert items into
    await groveDb.insert(
      rootTreePath,
      treeKey,
      { type: 'tree', value: Buffer.alloc(32) },
    );

    // Inserting an item into the subtree
    await groveDb.insert(
      itemTreePath,
      itemKey,
      { type: 'item', value: itemValue },
    );

    const element = await groveDb.get(itemTreePath, itemKey);

    expect(element.type).to.be.equal('item');
    expect(element.value).to.deep.equal(itemValue);
  });

  it('should store and delete a value', async () => {
    // Making a subtree to insert items into
    await groveDb.insert(
      rootTreePath,
      treeKey,
      { type: 'tree', value: Buffer.alloc(32) },
    );

    // Inserting an item into the subtree
    await groveDb.insert(
      itemTreePath,
      itemKey,
      { type: 'item', value: itemValue },
    );

    // Get item
    const element = await groveDb.get(itemTreePath, itemKey);

    expect(element.type).to.be.equal('item');
    expect(element.value).to.deep.equal(itemValue);

    // Delete an item from the subtree
    await groveDb.delete(
      itemTreePath,
      itemKey,
    );

    try {
      await groveDb.get(itemTreePath, itemKey);

      expect.fail('Expected to throw en error');
    } catch (e) {
      expect(e.message).to.be.equal('path key not found: key not found in Merk: 746573745f6b6579');
    }
  });

  describe('#startTransaction', () => {
    it('should not allow to read transactional data from main database until it\'s committed', async () => {
      // Making a subtree to insert items into
      await groveDb.insert(
        rootTreePath,
        treeKey,
        { type: 'tree', value: Buffer.alloc(32) },
      );

      await groveDb.startTransaction();

      // Inserting an item into the subtree
      await groveDb.insert(
        itemTreePath,
        itemKey,
        { type: 'item', value: itemValue },
        true,
      );

      // Inserted value is not yet commited, but can be retrieved by `get`
      // with `useTransaction` flag.
      const elementInTransaction = await groveDb.get(itemTreePath, itemKey, true);

      expect(elementInTransaction.type).to.be.equal('item');
      expect(elementInTransaction.value).to.deep.equal(itemValue);

      // ... and using `get` without the flag should return no value
      try {
        await groveDb.get(itemTreePath, itemKey);

        expect.fail('Expected to throw an error');
      } catch (e) {
        expect(e.message).to.be.equal('path key not found: key not found in Merk: 746573745f6b6579');
      }
    });
  });

  describe('#commitTransaction', () => {
    it('should commit transactional data to main database', async () => {
      // Making a subtree to insert items into
      await groveDb.insert(
        rootTreePath,
        treeKey,
        { type: 'tree', value: Buffer.alloc(32) },
      );

      await groveDb.startTransaction();

      // Inserting an item into the subtree
      await groveDb.insert(
        itemTreePath,
        itemKey,
        { type: 'item', value: itemValue },
        true,
      );

      // ... and using `get` without the flag should return no value
      try {
        await groveDb.get(itemTreePath, itemKey);

        expect.fail('Expected to throw an error');
      } catch (e) {
        expect(e.message).to.be.equal('path key not found: key not found in Merk: 746573745f6b6579');
      }

      await groveDb.commitTransaction();

      // When committed, the value should be accessible without running transaction
      const element = await groveDb.get(itemTreePath, itemKey);
      expect(element.type).to.be.equal('item');
      expect(element.value).to.deep.equal(itemValue);
    });
  });

  describe('#rollbackTransaction', () => {
    it('should rollaback transaction state to its initial state', async () => {
      // Making a subtree to insert items into
      await groveDb.insert(
        rootTreePath,
        treeKey,
        { type: 'tree', value: Buffer.alloc(32) },
      );

      await groveDb.startTransaction();

      // Inserting an item into the subtree
      await groveDb.insert(
        itemTreePath,
        itemKey,
        { type: 'item', value: itemValue },
        true,
      );

      // Should rollback inserted item
      await groveDb.rollbackTransaction();

      try {
        await groveDb.get(itemTreePath, itemKey);

        expect.fail('Expected to throw an error');
      } catch (e) {
        expect(e.message).to.be.equal('path key not found: key not found in Merk: 746573745f6b6579');
      }
    });
  });

  describe('#isTransactionStarted', () => {
    it('should return true if transaction is started', async () => {
      // Making a subtree to insert items into
      await groveDb.insert(
        rootTreePath,
        treeKey,
        { type: 'tree', value: Buffer.alloc(32) },
      );

      await groveDb.startTransaction();

      const result = await groveDb.isTransactionStarted();

      // eslint-disable-next-line no-unused-expressions
      expect(result).to.be.true;
    });

    it('should return false if transaction is not started', async () => {
      const result = await groveDb.isTransactionStarted();

      // eslint-disable-next-line no-unused-expressions
      expect(result).to.be.false;
    });
  });

  describe('#abortTransaction', () => {
    it('should abort transaction', async () => {
      // Making a subtree to insert items into
      await groveDb.insert(
        rootTreePath,
        treeKey,
        { type: 'tree', value: Buffer.alloc(32) },
      );

      await groveDb.startTransaction();

      // Inserting an item into the subtree
      await groveDb.insert(
        itemTreePath,
        itemKey,
        { type: 'item', value: itemValue },
        true,
      );

      // Should abort inserted item
      await groveDb.abortTransaction();

      const isTransactionStarted = await groveDb.isTransactionStarted();

      // eslint-disable-next-line no-unused-expressions
      expect(isTransactionStarted).to.be.false;

      try {
        await groveDb.get(itemTreePath, itemKey);

        expect.fail('Expected to throw an error');
      } catch (e) {
        expect(e.message).to.be.equal('path key not found: key not found in Merk: 746573745f6b6579');
      }
    });
  });

  describe('#insertIfNotExists', () => {
    it('should insert a value if key is not exist yet', async () => {
      // Making a subtree to insert items into
      await groveDb.insert(
        rootTreePath,
        treeKey,
        { type: 'tree', value: Buffer.alloc(32) },
      );

      // Inserting an item into the subtree
      await groveDb.insertIfNotExists(
        itemTreePath,
        itemKey,
        { type: 'item', value: itemValue },
      );

      const element = await groveDb.get(itemTreePath, itemKey);

      expect(element.type).to.equal('item');
      expect(element.value).to.deep.equal(itemValue);
    });

    it('shouldn\'t overwrite already stored value', async () => {
      // Making a subtree to insert items into
      await groveDb.insert(
        rootTreePath,
        treeKey,
        { type: 'tree', value: Buffer.alloc(32) },
      );

      // Inserting an item into the subtree
      await groveDb.insert(
        itemTreePath,
        itemKey,
        { type: 'item', value: itemValue },
      );

      const newItemValue = Buffer.from('replaced item value');

      // Inserting an item into the subtree
      await groveDb.insertIfNotExists(
        itemTreePath,
        itemKey,
        { type: 'item', value: newItemValue },
      );

      const element = await groveDb.get(itemTreePath, itemKey);

      expect(element.type).to.equal('item');
      expect(element.value).to.deep.equal(itemValue);
    });
  });

  describe('#insert', () => {
    it('should be able to insert a tree', async () => {
      await groveDb.insert(
        [],
        Buffer.from('test_tree'),
        { type: 'tree', value: Buffer.alloc(32) },
      );
    });

    it('should throw when trying to insert non-existent element type', async () => {
      const path = [];
      const key = Buffer.from('test_key');

      try {
        await groveDb.insert(
          path,
          key,
          { type: 'not_a_tree', value: Buffer.alloc(32) },
        );

        expect.fail('Expected to throw en error');
      } catch (e) {
        expect(e.message).to.be.equal('Unexpected element type not_a_tree');
      }
    });

    it('should throw when trying to insert a tree that is not 32 bytes', async () => {
      const path = [];
      const key = Buffer.from('test_key');

      try {
        await groveDb.insert(
          path,
          key,
          { type: 'tree', value: Buffer.alloc(1) },
        );

        expect.fail('Expected to throw en error');
      } catch (e) {
        expect(e.message).to.be.equal('Tree buffer is expected to be 32 bytes long, but got 1');
      }
    });
  });

  describe('auxiliary data methods', () => {
    let key;
    let value;

    beforeEach(() => {
      key = Buffer.from('aux_key');
      value = Buffer.from('ayy');
    });

    it('should be able to store and get aux data', async () => {
      await groveDb.putAux(key, value);

      const result = await groveDb.getAux(key);

      expect(result).to.deep.equal(value);
    });

    it('should be able to insert and delete aux data', async () => {
      await groveDb.putAux(key, value);

      await groveDb.deleteAux(key);

      const result = await groveDb.getAux(key);

      // eslint-disable-next-line no-unused-expressions
      expect(result).to.be.null;
    });
  });

  describe('#getPathQuery for Item subtrees', () => {
    let aValue;
    let aKey;
    let bValue;
    let bKey;
    let cValue;
    let cKey;

    beforeEach(async () => {
      await groveDb.insert(
        rootTreePath,
        treeKey,
        { type: 'tree', value: Buffer.alloc(32) },
      );

      aValue = Buffer.from('a');
      aKey = Buffer.from('aKey');
      bValue = Buffer.from('b');
      bKey = Buffer.from('bKey');
      cValue = Buffer.from('c');
      cKey = Buffer.from('cKey');

      await groveDb.insert(
        itemTreePath,
        aKey,
        { type: 'item', value: aValue },
      );

      await groveDb.insert(
        itemTreePath,
        bKey,
        { type: 'item', value: bValue },
      );

      await groveDb.insert(
        itemTreePath,
        cKey,
        { type: 'item', value: cValue },
      );
    });

    it('should be able to use limit', async () => {
      const query = {
        path: itemTreePath,
        query: {
          limit: 1,
          query: {
            items: [
              {
                type: 'rangeFrom',
                from: bValue,
              },
            ],
          },
        },
      };

      const result = await groveDb.getPathQuery(query);

      expect(result).to.have.a.lengthOf(2);

      const [elementValues, skipped] = result;

      expect(elementValues).to.deep.equals([
        bValue,
      ]);

      expect(skipped).to.equals(0);
    });

    it('should be able to use offset', async () => {
      const query = {
        path: itemTreePath,
        query: {
          offset: 1,
          query: {
            items: [
              {
                type: 'rangeFrom',
                from: bValue,
              },
            ],
          },

        },
      };

      const result = await groveDb.getPathQuery(query);

      expect(result).to.have.a.lengthOf(2);

      const [elementValues, skipped] = result;

      expect(elementValues).to.deep.equals([
        cValue,
      ]);

      expect(skipped).to.equals(1);
    });

    it('should be able to retrieve data using `key`', async () => {
      const query = {
        path: itemTreePath,
        query: {
          query: {
            items: [
              {
                type: 'key',
                key: aKey,
              },
              {
                type: 'key',
                key: bKey,
              },
            ],
          },
        },
      };

      const result = await groveDb.getPathQuery(query);

      expect(result).to.have.a.lengthOf(2);

      const [elementValues, skipped] = result;

      expect(elementValues).to.deep.equals([
        aValue,
        bValue,
      ]);

      expect(skipped).to.equals(0);
    });

    it('should be able to retrieve data using `range`', async () => {
      const query = {
        path: itemTreePath,
        query: {
          query: {
            items: [
              {
                type: 'range',
                from: aKey,
                to: cKey,
              },
            ],
          },

        },
      };

      const result = await groveDb.getPathQuery(query);

      expect(result).to.have.a.lengthOf(2);

      const [elementValues, skipped] = result;

      expect(elementValues).to.deep.equals([
        aValue,
        bValue,
      ]);

      expect(skipped).to.equals(0);
    });

    it('should be able to retrieve data using `rangeInclusive`', async () => {
      const query = {
        path: itemTreePath,
        query: {
          query: {
            items: [
              {
                type: 'rangeInclusive',
                from: aKey,
                to: bKey,
              },
            ],
          },
        },
      };

      const result = await groveDb.getPathQuery(query);

      expect(result).to.have.a.lengthOf(2);

      const [elementValues, skipped] = result;

      expect(elementValues).to.deep.equals([
        aValue,
        bValue,
      ]);

      expect(skipped).to.equals(0);
    });

    it('should be able to retrieve data using `rangeFull`', async () => {
      const query = {
        path: itemTreePath,
        query: {
          query: {
            items: [
              {
                type: 'rangeFull',
              },
            ],
          },

        },
      };

      const result = await groveDb.getPathQuery(query);

      expect(result).to.have.a.lengthOf(2);

      const [elementValues, skipped] = result;

      expect(elementValues).to.deep.equals([
        aValue,
        bValue,
        cValue,
      ]);

      expect(skipped).to.equals(0);
    });

    it('should be able to retrieve data using `rangeFrom`', async () => {
      const query = {
        path: itemTreePath,
        query: {
          query: {
            items: [
              {
                type: 'rangeFrom',
                from: bKey,
              },
            ],
          },
        },
      };

      const result = await groveDb.getPathQuery(query);

      expect(result).to.have.a.lengthOf(2);

      const [elementValues, skipped] = result;

      expect(elementValues).to.deep.equals([
        bValue,
        cValue,
      ]);

      expect(skipped).to.equals(0);
    });

    it('should be able to retrieve data using `rangeTo`', async () => {
      const query = {
        path: itemTreePath,
        query: {
          query: {
            items: [
              {
                type: 'rangeTo',
                to: cKey,
              },
            ],
          },

        },
      };

      const result = await groveDb.getPathQuery(query);

      expect(result).to.have.a.lengthOf(2);

      const [elementValues, skipped] = result;

      expect(elementValues).to.deep.equals([
        aValue,
        bValue,
      ]);

      expect(skipped).to.equals(0);
    });

    it('should be able to retrieve data using `rangeToInclusive`', async () => {
      const query = {
        path: itemTreePath,
        query: {
          query: {
            items: [
              {
                type: 'rangeToInclusive',
                to: cKey,
              },
            ],
          },
        },
      };

      const result = await groveDb.getPathQuery(query);

      expect(result).to.have.a.lengthOf(2);

      const [elementValues, skipped] = result;

      expect(elementValues).to.deep.equals([
        aValue,
        bValue,
        cValue,
      ]);

      expect(skipped).to.equals(0);
    });

    it('should be able to retrieve data using `rangeAfter`', async () => {
      const query = {
        path: itemTreePath,
        query: {
          query: {
            items: [
              {
                type: 'rangeAfter',
                after: aKey,
              },
            ],
          },

        },
      };

      const result = await groveDb.getPathQuery(query);

      expect(result).to.have.a.lengthOf(2);

      const [elementValues, skipped] = result;

      expect(elementValues).to.deep.equals([
        bValue,
        cValue,
      ]);

      expect(skipped).to.equals(0);
    });

    it('should be able to retrieve data using `rangeAfterTo`', async () => {
      const query = {
        path: itemTreePath,
        query: {
          query: {
            items: [
              {
                type: 'rangeAfterTo',
                after: aKey,
                to: cKey,
              },
            ],
          },
        },
      };

      const result = await groveDb.getPathQuery(query);

      expect(result).to.have.a.lengthOf(2);

      const [elementValues, skipped] = result;

      expect(elementValues).to.deep.equals([
        bValue,
      ]);

      expect(skipped).to.equals(0);
    });

    it('should be able to retrieve data using `rangeAfterToInclusive`', async () => {
      const query = {
        path: itemTreePath,
        query: {
          query: {
            items: [
              {
                type: 'rangeAfterToInclusive',
                after: aKey,
                to: cKey,
              },
            ],
          },

        },
      };

      const result = await groveDb.getPathQuery(query);

      expect(result).to.have.a.lengthOf(2);

      const [elementValues, skipped] = result;

      expect(elementValues).to.deep.equals([
        bValue,
        cValue,
      ]);

      expect(skipped).to.equals(0);
    });
  });

  describe('#getPathQuery for nested subtrees with subquery', () => {
    let dPath;
    let dKey;
    let ePath;

    let daValue;
    let dbValue;
    let dcValue;
    let eaValue;
    let eaKey;
    let ebValue;

    beforeEach(async () => {
      await groveDb.insert(
        rootTreePath,
        treeKey,
        { type: 'tree', value: Buffer.alloc(32) },
      );

      dKey = Buffer.from('dKey');
      daValue = Buffer.from('da');
      dbValue = Buffer.from('db');
      dcValue = Buffer.from('dc');
      eaValue = Buffer.from('ea');
      eaKey = Buffer.from('eaKey');
      ebValue = Buffer.from('eb');

      dPath = [...itemTreePath];
      dPath.push(dKey);
      await groveDb.insert(
        itemTreePath,
        dKey,
        { type: 'tree', value: Buffer.alloc(32) },
      );

      await groveDb.insert(
        dPath,
        Buffer.from('daKey'),
        { type: 'item', value: daValue },
      );

      await groveDb.insert(
        dPath,
        Buffer.from('dbKey'),
        { type: 'item', value: dbValue },
      );

      await groveDb.insert(
        dPath,
        Buffer.from('dcKey'),
        { type: 'item', value: dcValue },
      );

      const eKey = Buffer.from('eKey');
      ePath = [...itemTreePath];
      ePath.push(eKey);
      await groveDb.insert(
        itemTreePath,
        eKey,
        { type: 'tree', value: Buffer.alloc(32) },
      );

      await groveDb.insert(
        ePath,
        Buffer.from('eaKey'),
        { type: 'item', value: eaValue },
      );

      await groveDb.insert(
        ePath,
        Buffer.from('ebKey'),
        { type: 'item', value: ebValue },
      );
    });

    it('should be able to retrieve data with subquery', async () => {
      // This should give us only last subtree and apply subquery to it
      const query = {
        path: itemTreePath,
        query: {
          query: {
            items: [
              {
                type: 'rangeAfter',
                after: dKey,
              },
            ],
            subquery: {
              items: [
                {
                  type: 'rangeAfter',
                  after: eaKey,
                },
              ],
            },
          },
        },
      };

      const result = await groveDb.getPathQuery(query);

      expect(result).to.have.a.lengthOf(2);

      const [elementValues, skipped] = result;

      expect(elementValues).to.deep.equals([
        ebValue,
      ]);

      expect(skipped).to.equals(0);
    });
  });

  describe('#flush', () => {
    it('should flush data on disc', async () => {
      await groveDb.insert(
        [],
        Buffer.from('test_tree'),
        { type: 'tree', value: Buffer.alloc(32) },
      );

      await groveDb.flush();
    });
  });

  describe('#getRootHash', () => {
    it('should return empty root hash if there is no data', async () => {
      const result = await groveDb.getRootHash();

      expect(result).to.deep.equal(Buffer.alloc(32));

      // Get root hash for transaction too
      await groveDb.startTransaction();

      const transactionalResult = await groveDb.getRootHash(true);

      expect(transactionalResult).to.deep.equal(Buffer.alloc(32));
    });

    it('should root hash', async () => {
      // Making a subtree to insert items into
      await groveDb.insert(
        rootTreePath,
        treeKey,
        { type: 'tree', value: Buffer.alloc(32) },
      );

      // Inserting an item into the subtree
      await groveDb.insert(
        itemTreePath,
        itemKey,
        { type: 'item', value: itemValue },
      );

      await groveDb.startTransaction();

      // Inserting an item into the subtree
      await groveDb.insert(
        itemTreePath,
        Buffer.from('transactional_test_key'),
        { type: 'item', value: itemValue },
        true,
      );

      const result = await groveDb.getRootHash();
      const transactionalResult = await groveDb.getRootHash(true);

      // Hashes shouldn't be equal
      expect(result).to.not.deep.equal(transactionalResult);

      // Hashes shouldn't be empty

      // eslint-disable-next-line no-unused-expressions
      expect(result >= Buffer.alloc(32)).to.be.true;

      // eslint-disable-next-line no-unused-expressions
      expect(transactionalResult >= Buffer.alloc(32)).to.be.true;
    });
  });
});
