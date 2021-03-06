// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "txdb.h"
#include "masternodes/masternodes.h"
#include "masternodes/dpos_p2p_messages.h"

#include "chainparams.h"
#include "hash.h"
#include "main.h"
#include "pow.h"
#include "uint256.h"

#include <stdint.h>

#include <boost/thread.hpp>

using namespace std;

// NOTE: Per issue #3277, do not use the prefix 'X' or 'x' as they were
// previously used by DB_SAPLING_ANCHOR and DB_BEST_SAPLING_ANCHOR.

// Prefixes for the coin database (chainstate/)
static const char DB_SPROUT_ANCHOR = 'A';
static const char DB_SAPLING_ANCHOR = 'Z';
static const char DB_NULLIFIER = 's';
static const char DB_SAPLING_NULLIFIER = 'S';
static const char DB_COINS = 'c';
static const char DB_BEST_BLOCK = 'B';
static const char DB_BEST_SPROUT_ANCHOR = 'a';
static const char DB_BEST_SAPLING_ANCHOR = 'z';

// Prefixes to the block database (blocks/index/)
static const char DB_BLOCK_FILES = 'f';
static const char DB_TXINDEX = 't';
static const char DB_BLOCK_INDEX = 'b';
static const char DB_FLAG = 'F';
static const char DB_REINDEX_FLAG = 'R';
static const char DB_LAST_BLOCK = 'l';

// Prefixes to the masternodes database (masternodes/)
static const char DB_MASTERNODES = 'M';
static const char DB_MASTERNODESUNDO = 'U';
static const char DB_SETOPERATORUNDO = 'u';
static const char DB_DISMISSVOTES = 'V';
static const char DB_TEAM = 'T';
// insightexplorer
static const char DB_ADDRESSINDEX = 'd';
static const char DB_ADDRESSUNSPENTINDEX = 'u';
static const char DB_SPENTINDEX = 'p';
static const char DB_TIMESTAMPINDEX = 'T';
static const char DB_BLOCKHASHINDEX = 'h';
static const char DB_PRUNEDEAD = 'D';
static const char DB_MN_HEIGHT = 'H';

// Prefixes to the dpos database (dpos/)
static const char DB_DPOS_TX_VOTES = 't';
static const char DB_DPOS_ROUND_VOTES = 'p';
static const char DB_DPOS_VICE_BLOCKS = 'b';

namespace
{
    template<typename Key, typename Value>
    void dbWrite(CDBWrapper * db, Key key, Value value, CDBBatch * batch, bool fsync = false)
    {
        assert(db != nullptr);

        if (batch == nullptr) {
            db->Write(key, value, fsync);
        } else {
            batch->Write(key, value);
        }
    }

    template<typename Key>
    void dbErase(CDBWrapper * db, Key key, CDBBatch * batch, bool fsync = false)
    {
        assert(db != nullptr);

        if (batch == nullptr) {
            db->Erase(key, fsync);
        } else {
            batch->Erase(key);
        }
    }
}

CCoinsViewDB::CCoinsViewDB(std::string dbName, size_t nCacheSize, bool fMemory, bool fWipe) : db(GetDataDir() / dbName, nCacheSize, fMemory, fWipe) {
}

CCoinsViewDB::CCoinsViewDB(size_t nCacheSize, bool fMemory, bool fWipe) : db(GetDataDir() / "chainstate", nCacheSize, fMemory, fWipe) 
{
}

bool CCoinsViewDB::GetSproutAnchorAt(const uint256 &rt, SproutMerkleTree &tree) const {
    if (rt == SproutMerkleTree::empty_root()) {
        SproutMerkleTree new_tree;
        tree = new_tree;
        return true;
    }

    bool read = db.Read(make_pair(DB_SPROUT_ANCHOR, rt), tree);

    return read;
}

bool CCoinsViewDB::GetSaplingAnchorAt(const uint256 &rt, SaplingMerkleTree &tree) const {
    if (rt == SaplingMerkleTree::empty_root()) {
        SaplingMerkleTree new_tree;
        tree = new_tree;
        return true;
    }

    bool read = db.Read(make_pair(DB_SAPLING_ANCHOR, rt), tree);

    return read;
}

bool CCoinsViewDB::GetNullifier(const uint256 &nf, ShieldedType type) const {
    bool spent = false;
    char dbChar;
    switch (type) {
        case SPROUT:
            dbChar = DB_NULLIFIER;
            break;
        case SAPLING:
            dbChar = DB_SAPLING_NULLIFIER;
            break;
        default:
            throw runtime_error("Unknown shielded type");
    }
    return db.Read(make_pair(dbChar, nf), spent);
}

bool CCoinsViewDB::GetCoins(const uint256 &txid, CCoins &coins) const {
    return db.Read(make_pair(DB_COINS, txid), coins);
}

bool CCoinsViewDB::HaveCoins(const uint256 &txid) const {
    return db.Exists(make_pair(DB_COINS, txid));
}

uint256 CCoinsViewDB::GetBestBlock() const {
    uint256 hashBestChain;
    if (!db.Read(DB_BEST_BLOCK, hashBestChain))
        return uint256();
    return hashBestChain;
}

uint256 CCoinsViewDB::GetBestAnchor(ShieldedType type) const {
    uint256 hashBestAnchor;
    
    switch (type) {
        case SPROUT:
            if (!db.Read(DB_BEST_SPROUT_ANCHOR, hashBestAnchor))
                return SproutMerkleTree::empty_root();
            break;
        case SAPLING:
            if (!db.Read(DB_BEST_SAPLING_ANCHOR, hashBestAnchor))
                return SaplingMerkleTree::empty_root();
            break;
        default:
            throw runtime_error("Unknown shielded type");
    }

    return hashBestAnchor;
}

void BatchWriteNullifiers(CDBBatch& batch, CNullifiersMap& mapToUse, const char& dbChar)
{
    for (CNullifiersMap::iterator it = mapToUse.begin(); it != mapToUse.end();) {
        if (it->second.flags & CNullifiersCacheEntry::DIRTY) {
            if (!it->second.entered)
                batch.Erase(make_pair(dbChar, it->first));
            else
                batch.Write(make_pair(dbChar, it->first), true);
            // TODO: changed++? ... See comment in CCoinsViewDB::BatchWrite. If this is needed we could return an int
        }
        CNullifiersMap::iterator itOld = it++;
        mapToUse.erase(itOld);
    }
}

template<typename Map, typename MapIterator, typename MapEntry, typename Tree>
void BatchWriteAnchors(CDBBatch& batch, Map& mapToUse, const char& dbChar)
{
    for (MapIterator it = mapToUse.begin(); it != mapToUse.end();) {
        if (it->second.flags & MapEntry::DIRTY) {
            if (!it->second.entered)
                batch.Erase(make_pair(dbChar, it->first));
            else {
                if (it->first != Tree::empty_root()) {
                    batch.Write(make_pair(dbChar, it->first), it->second.tree);
                }
            }
            // TODO: changed++?
        }
        MapIterator itOld = it++;
        mapToUse.erase(itOld);
    }
}

bool CCoinsViewDB::BatchWrite(CCoinsMap &mapCoins,
                              const uint256 &hashBlock,
                              const uint256 &hashSproutAnchor,
                              const uint256 &hashSaplingAnchor,
                              CAnchorsSproutMap &mapSproutAnchors,
                              CAnchorsSaplingMap &mapSaplingAnchors,
                              CNullifiersMap &mapSproutNullifiers,
                              CNullifiersMap &mapSaplingNullifiers) {
    CDBBatch batch(db);
    size_t count = 0;
    size_t changed = 0;
    for (CCoinsMap::iterator it = mapCoins.begin(); it != mapCoins.end();) {
        if (it->second.flags & CCoinsCacheEntry::DIRTY) {
            if (it->second.coins.IsPruned())
                batch.Erase(make_pair(DB_COINS, it->first));
            else
                batch.Write(make_pair(DB_COINS, it->first), it->second.coins);
            changed++;
        }
        count++;
        CCoinsMap::iterator itOld = it++;
        mapCoins.erase(itOld);
    }

    ::BatchWriteAnchors<CAnchorsSproutMap, CAnchorsSproutMap::iterator, CAnchorsSproutCacheEntry, SproutMerkleTree>(batch, mapSproutAnchors, DB_SPROUT_ANCHOR);
    ::BatchWriteAnchors<CAnchorsSaplingMap, CAnchorsSaplingMap::iterator, CAnchorsSaplingCacheEntry, SaplingMerkleTree>(batch, mapSaplingAnchors, DB_SAPLING_ANCHOR);

    ::BatchWriteNullifiers(batch, mapSproutNullifiers, DB_NULLIFIER);
    ::BatchWriteNullifiers(batch, mapSaplingNullifiers, DB_SAPLING_NULLIFIER);

    if (!hashBlock.IsNull())
        batch.Write(DB_BEST_BLOCK, hashBlock);
    if (!hashSproutAnchor.IsNull())
        batch.Write(DB_BEST_SPROUT_ANCHOR, hashSproutAnchor);
    if (!hashSaplingAnchor.IsNull())
        batch.Write(DB_BEST_SAPLING_ANCHOR, hashSaplingAnchor);

    LogPrint("coindb", "Committing %u changed transactions (out of %u) to coin database...\n", (unsigned int)changed, (unsigned int)count);
    return db.WriteBatch(batch);
}

bool CCoinsViewDB::GetStats(CCoinsStats &stats) const {
    /* It seems that there are no "const iterators" for LevelDB.  Since we
       only need read operations on it, use a const-cast to get around
       that restriction.  */
    boost::scoped_ptr<CDBIterator> pcursor(const_cast<CDBWrapper*>(&db)->NewIterator());
    pcursor->Seek(DB_COINS);

    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    stats.hashBlock = GetBestBlock();
    ss << stats.hashBlock;
    CAmount nTotalAmount = 0;
    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        std::pair<char, uint256> key;
        CCoins coins;
        if (pcursor->GetKey(key) && key.first == DB_COINS) {
            if (pcursor->GetValue(coins)) {
                stats.nTransactions++;
                for (unsigned int i=0; i<coins.vout.size(); i++) {
                    const CTxOut &out = coins.vout[i];
                    if (!out.IsNull()) {
                        stats.nTransactionOutputs++;
                        ss << VARINT(i+1);
                        ss << out;
                        nTotalAmount += out.nValue;
                    }
                }
                stats.nSerializedSize += 32 + pcursor->GetValueSize();
                ss << VARINT(0);
            } else {
                return error("CCoinsViewDB::GetStats() : unable to read value");
            }
        } else {
            break;
        }
        pcursor->Next();
    }
    {
        LOCK(cs_main);
        stats.nHeight = mapBlockIndex.find(stats.hashBlock)->second->nHeight;
    }
    stats.hashSerialized = ss.GetHash();
    stats.nTotalAmount = nTotalAmount;
    return true;
}

CBlockTreeDB::CBlockTreeDB(size_t nCacheSize, bool fMemory, bool fWipe) : CDBWrapper(GetDataDir() / "blocks" / "index", nCacheSize, fMemory, fWipe) {
}

bool CBlockTreeDB::ReadBlockFileInfo(int nFile, CBlockFileInfo &info) {
    return Read(make_pair(DB_BLOCK_FILES, nFile), info);
}

bool CBlockTreeDB::WriteReindexing(bool fReindexing) {
    if (fReindexing)
        return Write(DB_REINDEX_FLAG, '1');
    else
        return Erase(DB_REINDEX_FLAG);
}

bool CBlockTreeDB::ReadReindexing(bool &fReindexing) {
    fReindexing = Exists(DB_REINDEX_FLAG);
    return true;
}

bool CBlockTreeDB::ReadLastBlockFile(int &nFile) {
    return Read(DB_LAST_BLOCK, nFile);
}

bool CBlockTreeDB::WriteBatchSync(const std::vector<std::pair<int, const CBlockFileInfo*> >& fileInfo, int nLastFile, const std::vector<const CBlockIndex*>& blockinfo) {
    CDBBatch batch(*this);
    for (std::vector<std::pair<int, const CBlockFileInfo*> >::const_iterator it=fileInfo.begin(); it != fileInfo.end(); it++) {
        batch.Write(make_pair(DB_BLOCK_FILES, it->first), *it->second);
    }
    batch.Write(DB_LAST_BLOCK, nLastFile);
    for (std::vector<const CBlockIndex*>::const_iterator it=blockinfo.begin(); it != blockinfo.end(); it++) {
        batch.Write(make_pair(DB_BLOCK_INDEX, (*it)->GetBlockHash()), CDiskBlockIndex(*it));
    }
    return WriteBatch(batch, true);
}

bool CBlockTreeDB::EraseBatchSync(const std::vector<const CBlockIndex*>& blockinfo) {
    CDBBatch batch(*this);
    for (std::vector<const CBlockIndex*>::const_iterator it=blockinfo.begin(); it != blockinfo.end(); it++) {
        batch.Erase(make_pair(DB_BLOCK_INDEX, (*it)->GetBlockHash()));
    }
    return WriteBatch(batch, true);
}

bool CBlockTreeDB::ReadTxIndex(const uint256 &txid, CDiskTxPos &pos) {
    return Read(make_pair(DB_TXINDEX, txid), pos);
}

bool CBlockTreeDB::WriteTxIndex(const std::vector<std::pair<uint256, CDiskTxPos> >&vect) {
    CDBBatch batch(*this);
    for (std::vector<std::pair<uint256,CDiskTxPos> >::const_iterator it=vect.begin(); it!=vect.end(); it++)
        batch.Write(make_pair(DB_TXINDEX, it->first), it->second);
    return WriteBatch(batch);
}

// START insightexplorer
// https://github.com/bitpay/bitcoin/commit/017f548ea6d89423ef568117447e61dd5707ec42#diff-81e4f16a1b5d5b7ca25351a63d07cb80R183
bool CBlockTreeDB::UpdateAddressUnspentIndex(const std::vector<CAddressUnspentDbEntry> &vect)
{
    CDBBatch batch(*this);
    for (std::vector<CAddressUnspentDbEntry>::const_iterator it=vect.begin(); it!=vect.end(); it++) {
        if (it->second.IsNull()) {
            batch.Erase(make_pair(DB_ADDRESSUNSPENTINDEX, it->first));
        } else {
            batch.Write(make_pair(DB_ADDRESSUNSPENTINDEX, it->first), it->second);
        }
    }
    return WriteBatch(batch);
}

bool CBlockTreeDB::ReadAddressUnspentIndex(uint160 addressHash, int type, std::vector<CAddressUnspentDbEntry> &unspentOutputs)
{
    boost::scoped_ptr<CDBIterator> pcursor(NewIterator());

    pcursor->Seek(make_pair(DB_ADDRESSUNSPENTINDEX, CAddressIndexIteratorKey(type, addressHash)));

    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        std::pair<char,CAddressUnspentKey> key;
        if (!(pcursor->GetKey(key) && key.first == DB_ADDRESSUNSPENTINDEX && key.second.hashBytes == addressHash))
            break;
        CAddressUnspentValue nValue;
        if (!pcursor->GetValue(nValue))
            return error("failed to get address unspent value");
        unspentOutputs.push_back(make_pair(key.second, nValue));
        pcursor->Next();
    }
    return true;
}

bool CBlockTreeDB::WriteAddressIndex(const std::vector<CAddressIndexDbEntry> &vect) {
    CDBBatch batch(*this);
    for (std::vector<CAddressIndexDbEntry>::const_iterator it=vect.begin(); it!=vect.end(); it++)
        batch.Write(make_pair(DB_ADDRESSINDEX, it->first), it->second);
    return WriteBatch(batch);
}

bool CBlockTreeDB::EraseAddressIndex(const std::vector<CAddressIndexDbEntry> &vect) {
    CDBBatch batch(*this);
    for (std::vector<CAddressIndexDbEntry>::const_iterator it=vect.begin(); it!=vect.end(); it++)
        batch.Erase(make_pair(DB_ADDRESSINDEX, it->first));
    return WriteBatch(batch);
}

bool CBlockTreeDB::ReadAddressIndex(
        uint160 addressHash, int type,
        std::vector<CAddressIndexDbEntry> &addressIndex,
        int start, int end)
{
    boost::scoped_ptr<CDBIterator> pcursor(NewIterator());

    if (start > 0 && end > 0) {
        pcursor->Seek(make_pair(DB_ADDRESSINDEX, CAddressIndexIteratorHeightKey(type, addressHash, start)));
    } else {
        pcursor->Seek(make_pair(DB_ADDRESSINDEX, CAddressIndexIteratorKey(type, addressHash)));
    }

    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        std::pair<char,CAddressIndexKey> key;
        if (!(pcursor->GetKey(key) && key.first == DB_ADDRESSINDEX && key.second.hashBytes == addressHash))
            break;
        if (end > 0 && key.second.blockHeight > end)
            break;
        CAmount nValue;
        if (!pcursor->GetValue(nValue))
            return error("failed to get address index value");
        addressIndex.push_back(make_pair(key.second, nValue));
        pcursor->Next();
    }
    return true;
}

bool CBlockTreeDB::ReadSpentIndex(CSpentIndexKey &key, CSpentIndexValue &value) {
    return Read(make_pair(DB_SPENTINDEX, key), value);
}

bool CBlockTreeDB::UpdateSpentIndex(const std::vector<CSpentIndexDbEntry> &vect) {
    CDBBatch batch(*this);
    for (std::vector<CSpentIndexDbEntry>::const_iterator it=vect.begin(); it!=vect.end(); it++) {
        if (it->second.IsNull()) {
            batch.Erase(make_pair(DB_SPENTINDEX, it->first));
        } else {
            batch.Write(make_pair(DB_SPENTINDEX, it->first), it->second);
        }
    }
    return WriteBatch(batch);
}

bool CBlockTreeDB::WriteTimestampIndex(const CTimestampIndexKey &timestampIndex) {
    CDBBatch batch(*this);
    batch.Write(make_pair(DB_TIMESTAMPINDEX, timestampIndex), 0);
    return WriteBatch(batch);
}

bool CBlockTreeDB::ReadTimestampIndex(unsigned int high, unsigned int low,
    const bool fActiveOnly, std::vector<std::pair<uint256, unsigned int> > &hashes)
{
    boost::scoped_ptr<CDBIterator> pcursor(NewIterator());

    pcursor->Seek(make_pair(DB_TIMESTAMPINDEX, CTimestampIndexIteratorKey(low)));

    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        std::pair<char, CTimestampIndexKey> key;
        if (!(pcursor->GetKey(key) && key.first == DB_TIMESTAMPINDEX && key.second.timestamp < high)) {
            break;
        }
        if (fActiveOnly) {
            CBlockIndex* pblockindex = mapBlockIndex[key.second.blockHash];
            if (chainActive.Contains(pblockindex)) {
                hashes.push_back(std::make_pair(key.second.blockHash, key.second.timestamp));
            }
        } else {
            hashes.push_back(std::make_pair(key.second.blockHash, key.second.timestamp));
        }
        pcursor->Next();
    }
    return true;
}

bool CBlockTreeDB::WriteTimestampBlockIndex(const CTimestampBlockIndexKey &blockhashIndex,
    const CTimestampBlockIndexValue &logicalts)
{
    CDBBatch batch(*this);
    batch.Write(make_pair(DB_BLOCKHASHINDEX, blockhashIndex), logicalts);
    return WriteBatch(batch);
}

bool CBlockTreeDB::ReadTimestampBlockIndex(const uint256 &hash, unsigned int &ltimestamp)
{
    CTimestampBlockIndexValue(lts);
    if (!Read(std::make_pair(DB_BLOCKHASHINDEX, hash), lts))
        return false;

    ltimestamp = lts.ltimestamp;
    return true;
}
// END insightexplorer

bool CBlockTreeDB::WriteFlag(const std::string &name, bool fValue) {
    return Write(std::make_pair(DB_FLAG, name), fValue ? '1' : '0');
}

bool CBlockTreeDB::ReadFlag(const std::string &name, bool &fValue) {
    char ch;
    if (!Read(std::make_pair(DB_FLAG, name), ch))
        return false;
    fValue = ch == '1';
    return true;
}

bool CBlockTreeDB::LoadBlockIndexGuts(boost::function<CBlockIndex*(const uint256&)> insertBlockIndex)
{
    boost::scoped_ptr<CDBIterator> pcursor(NewIterator());

    pcursor->Seek(make_pair(DB_BLOCK_INDEX, uint256()));

    // Load mapBlockIndex
    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        std::pair<char, uint256> key;
        if (pcursor->GetKey(key) && key.first == DB_BLOCK_INDEX) {
            CDiskBlockIndex diskindex;
            if (pcursor->GetValue(diskindex)) {
                // Construct block index object
                CBlockIndex* pindexNew = insertBlockIndex(diskindex.GetBlockHash());
                pindexNew->pprev          = insertBlockIndex(diskindex.hashPrev);
                pindexNew->nHeight        = diskindex.nHeight;
                pindexNew->nFile          = diskindex.nFile;
                pindexNew->nDataPos       = diskindex.nDataPos;
                pindexNew->nUndoPos       = diskindex.nUndoPos;
                pindexNew->hashSproutAnchor     = diskindex.hashSproutAnchor;
                pindexNew->nVersion       = diskindex.nVersion;
                pindexNew->hashMerkleRoot = diskindex.hashMerkleRoot;
                pindexNew->hashFinalSaplingRoot   = diskindex.hashFinalSaplingRoot;
                pindexNew->nTime          = diskindex.nTime;
                pindexNew->nBits          = diskindex.nBits;
                pindexNew->nRound         = diskindex.nRound;
                pindexNew->nNonce         = diskindex.nNonce;
                pindexNew->nSolution      = diskindex.nSolution;
                pindexNew->nStatus        = diskindex.nStatus;
                pindexNew->nCachedBranchId = diskindex.nCachedBranchId;
                pindexNew->nTx            = diskindex.nTx;
                pindexNew->nSproutValue   = diskindex.nSproutValue;
                pindexNew->nSaplingValue  = diskindex.nSaplingValue;

                // Consistency checks
                auto header = pindexNew->GetBlockHeader();
                if (header.GetHash() != pindexNew->GetBlockHash())
                    return error("LoadBlockIndex(): block header inconsistency detected: on-disk = %s, in-memory = %s",
                       diskindex.ToString(),  pindexNew->ToString());
                if (!CheckProofOfWork(pindexNew->GetBlockHash(), pindexNew->nBits, Params().GetConsensus()))
                    return error("LoadBlockIndex(): CheckProofOfWork failed: %s", pindexNew->ToString());

                pcursor->Next();
            } else {
                return error("LoadBlockIndex() : failed to read value");
            }
        } else {
            break;
        }
    }

    return true;
}


CMasternodesViewDB::CMasternodesViewDB(size_t nCacheSize, bool fMemory, bool fWipe)
    : db(new CDBWrapper(GetDataDir() / "masternodes", nCacheSize, fMemory, fWipe))
{
}

// for test purposes only
CMasternodesViewDB::CMasternodesViewDB()
    : db(nullptr)
{
}

void CMasternodesViewDB::CommitBatch()
{
    if (batch)
    {
        db->WriteBatch(*batch);
        batch.reset();
    }
}

void CMasternodesViewDB::DropBatch()
{
    if (batch)
    {
        batch.reset();
    }
}

bool CMasternodesViewDB::ReadHeight(int & h)
{
    // it's a hack, cause we don't know active chain tip at the loading time
    if (!db->Read(DB_MN_HEIGHT, h))
    {
        h = 0;
    }
    return true;
}

void CMasternodesViewDB::WriteHeight(int h)
{
    BatchWrite(DB_MN_HEIGHT, h);
}

void CMasternodesViewDB::WriteMasternode(uint256 const & txid, CMasternode const & node)
{
    BatchWrite(make_pair(DB_MASTERNODES, txid), node);
}

void CMasternodesViewDB::EraseMasternode(uint256 const & txid)
{
    BatchErase(make_pair(DB_MASTERNODES, txid));
}

void CMasternodesViewDB::WriteVote(uint256 const & txid, CDismissVote const & vote)
{
    BatchWrite(make_pair(DB_DISMISSVOTES, txid), vote);
}

void CMasternodesViewDB::EraseVote(uint256 const & txid)
{
    BatchErase(make_pair(DB_DISMISSVOTES, txid));
}

void CMasternodesViewDB::WriteDeadIndex(int height, uint256 const & txid, char type)
{
    BatchWrite(make_pair(make_pair(DB_PRUNEDEAD, static_cast<int32_t>(height)), txid), type);
}

void CMasternodesViewDB::EraseDeadIndex(int height, uint256 const & txid)
{
    BatchErase(make_pair(make_pair(DB_PRUNEDEAD, static_cast<int32_t>(height)), txid));
}

void CMasternodesViewDB::WriteUndo(int height, uint256 const & txid, uint256 const & affectedItem, char undoType)
{
    BatchWrite(make_pair(make_pair(DB_MASTERNODESUNDO, static_cast<int32_t>(height)), make_pair(txid, affectedItem)), undoType);
}

void CMasternodesViewDB::EraseUndo(int height, uint256 const & txid, uint256 const & affectedItem)
{
    BatchErase(make_pair(make_pair(DB_MASTERNODESUNDO, static_cast<int32_t>(height)), make_pair(txid, affectedItem)));
}

void CMasternodesViewDB::ReadOperatorUndo(const uint256 & txid, COperatorUndoRec & value)
{
    db->Read(make_pair(DB_SETOPERATORUNDO, txid), value);
}

void CMasternodesViewDB::WriteOperatorUndo(uint256 const & txid, COperatorUndoRec const & value)
{
    BatchWrite(make_pair(DB_SETOPERATORUNDO, txid), value);
}

void CMasternodesViewDB::EraseOperatorUndo(uint256 const & txid)
{
    BatchErase(make_pair(DB_SETOPERATORUNDO, txid));
}

void CMasternodesViewDB::WriteTeam(int blockHeight, const CTeam & team)
{
    // we are sure that we have no spoiled records (all of them are deleted)
    for (CTeam::const_iterator it = team.begin(); it != team.end(); ++it)
    {
        BatchWrite(make_pair(make_pair(DB_TEAM, static_cast<int32_t>(blockHeight)), it->first), make_pair(it->second.joinHeight, it->second.operatorAuth));
    }
}

bool CMasternodesViewDB::LoadMasternodes(std::function<void(uint256 &, CMasternode &)> onNode) const
{
    boost::scoped_ptr<CDBIterator> pcursor(const_cast<CDBWrapper*>(&*db)->NewIterator());
    pcursor->Seek(DB_MASTERNODES);

    while (pcursor->Valid())
    {
        boost::this_thread::interruption_point();
        std::pair<char, uint256> key;
        if (pcursor->GetKey(key) && key.first == DB_MASTERNODES)
        {
            CMasternode node;
            if (pcursor->GetValue(node))
            {
                onNode(key.second, node);
            }
            else
            {
                return error("CMasternodesDB::LoadMasternodes() : unable to read value");
            }
        }
        else
        {
            break;
        }
        pcursor->Next();
    }
    return true;
}

bool CMasternodesViewDB::LoadVotes(std::function<void(uint256 const &, CDismissVote const &)> onVote) const
{
    boost::scoped_ptr<CDBIterator> pcursor(const_cast<CDBWrapper*>(&*db)->NewIterator());
    pcursor->Seek(DB_DISMISSVOTES);

    while (pcursor->Valid())
    {
        boost::this_thread::interruption_point();
        std::pair<char, uint256> key;
        if (pcursor->GetKey(key) && key.first == DB_DISMISSVOTES)
        {
            CDismissVote vote;
            if (pcursor->GetValue(vote))
            {
                onVote(key.second, vote);
            }
            else
            {
                return error("CMasternodesDB::LoadVotes() : unable to read value");
            }
        }
        else
        {
            break;
        }
        pcursor->Next();
    }
    return true;
}

bool CMasternodesViewDB::LoadUndo(std::function<void(int, uint256 const &, uint256 const &, char)> onUndo) const
{
    boost::scoped_ptr<CDBIterator> pcursor(const_cast<CDBWrapper*>(&*db)->NewIterator());
    pcursor->Seek(DB_MASTERNODESUNDO);

    while (pcursor->Valid())
    {
        boost::this_thread::interruption_point();
        // key = make_pair(make_pair(DB_MASTERNODESUNDO, static_cast<uint32_t>(height)), make_pair(txid, affectedItem))
        std::pair<std::pair<char, int32_t>, std::pair<uint256, uint256> > key;
        if (pcursor->GetKey(key) && key.first.first == DB_MASTERNODESUNDO)
        {
            char undoType;
            if (pcursor->GetValue(undoType))
            {
                onUndo(key.first.second, key.second.first, key.second.second, undoType);
            }
            else
            {
                return error("CMasternodesDB::LoadUndo() : unable to read value");
            }
        }
        else
        {
            break;
        }
        pcursor->Next();
    }
    return true;
}

bool CMasternodesViewDB::LoadTeams(CTeams & newteams) const
{
    newteams.clear();
    boost::scoped_ptr<CDBIterator> pcursor(const_cast<CDBWrapper*>(&*db)->NewIterator());
    pcursor->Seek(DB_TEAM);

    while (pcursor->Valid())
    {
        boost::this_thread::interruption_point();
        std::pair<std::pair<char, int32_t>, uint256> key;
        if (pcursor->GetKey(key) && key.first.first == DB_TEAM)
        {
            int32_t const & blockHeight = key.first.second;
            std::pair<int32_t, CKeyID> value;
            if (pcursor->GetValue(value))
            {
                newteams[blockHeight].insert(make_pair(key.second, TeamData { value.first, value.second }));
            }
            else
            {
                return error("CMasternodesDB::LoadTeams() : unable to read value");
            }
        }
        else
        {
            break;
        }
        pcursor->Next();
    }
    return true;
}

/*
 * Loads all data from DB, creates indexes, calculates voting counters
 */
bool CMasternodesViewDB::Load()
{
    Clear();

    bool result = true;
    result = result && ReadHeight(lastHeight);

    // Load masternodes itself, creating indexes
    result = result && LoadMasternodes([this] (uint256 & nodeId, CMasternode & node)
    {
        node.dismissVotesFrom = 0;
        node.dismissVotesAgainst = 0;
        allNodes.insert(std::make_pair(nodeId, node));
        nodesByOwner.insert(std::make_pair(node.ownerAuthAddress, nodeId));
        nodesByOperator.insert(std::make_pair(node.operatorAuthAddress, nodeId));

        if (node.IsActive())
        {
            activeNodes.insert(nodeId);
        }
    });

    // Load dismiss votes and update voting counters
    result = result && LoadVotes([this] (uint256 const & voteId, CDismissVote const & vote)
    {
        votes.insert(std::make_pair(voteId, vote));

        if (vote.IsActive())
        {
            // Indexing only active votes
            votesFrom.insert(std::make_pair(vote.from, voteId));
            votesAgainst.insert(std::make_pair(vote.against, voteId));

            // Assumed that node exists
            ++allNodes.at(vote.from).dismissVotesFrom;
            ++allNodes.at(vote.against).dismissVotesAgainst;
        }
    });

    // Load undo information
    result = result && LoadUndo([this] (int height, uint256 const & txid, uint256 const & affectedItem, char undoType)
    {
        txsUndo.insert(std::make_pair(std::make_pair(height, txid), std::make_pair(affectedItem, static_cast<MasternodesTxType>(undoType))));

        // There is the second way: load all 'operator undo' in different loop, but i think here is more "consistent"
        if (undoType == static_cast<char>(MasternodesTxType::SetOperatorReward))
        {
            COperatorUndoRec rec;
            ReadOperatorUndo(txid, rec);
            operatorUndo.insert(std::make_pair(txid, rec));
        }
    });

    // Load teams information
    result = result && LoadTeams(teams);

    if (result)
        LogPrintf("MN: db loaded: last height: %d; masternodes: %d; votes: %d; common undo: %d; operator undo: %d; teams: %d\n", lastHeight, allNodes.size(), votes.size(), txsUndo.size(), operatorUndo.size(), teams.size());
    else {
        LogPrintf("MN: fail to load database!");
    }
    return result;
}

bool CMasternodesViewDB::Flush()
{
    if (lastHeight < Params().GetConsensus().vUpgrades[Consensus::UPGRADE_SAPLING].nActivationHeight)
    {
        return true;
    }

    batch.reset();
    {
        boost::scoped_ptr<CDBIterator> pcursor(const_cast<CDBWrapper*>(&*db)->NewIterator());
        pcursor->Seek(DB_MASTERNODES);
        while (pcursor->Valid())
        {
            boost::this_thread::interruption_point();
            std::pair<char, uint256> key;
            if (pcursor->GetKey(key) && key.first == DB_MASTERNODES)
            {
                if (allNodes.find(key.second) == allNodes.end())
                {
                    BatchErase(key);
                }
            }
            else
            {
                break;
            }
            pcursor->Next();
        }
    }
    {
        boost::scoped_ptr<CDBIterator> pcursor(const_cast<CDBWrapper*>(&*db)->NewIterator());
        pcursor->Seek(DB_DISMISSVOTES);

        while (pcursor->Valid())
        {
            boost::this_thread::interruption_point();
            std::pair<char, uint256> key;
            if (pcursor->GetKey(key) && key.first == DB_DISMISSVOTES)
            {
                if (votes.find(key.second) == votes.end())
                {
                    BatchErase(key);
                }
            }
            else
            {
                break;
            }
            pcursor->Next();
        }
    }
    {
        boost::scoped_ptr<CDBIterator> pcursor(const_cast<CDBWrapper*>(&*db)->NewIterator());
        pcursor->Seek(DB_MASTERNODESUNDO);
        while (pcursor->Valid())
        {
            boost::this_thread::interruption_point();
            // key = make_pair(make_pair(DB_MASTERNODESUNDO, static_cast<uint32_t>(height)), make_pair(txid, affectedItem))
            std::pair<std::pair<char, int32_t>, std::pair<uint256, uint256> > key;
            if (pcursor->GetKey(key) && key.first.first == DB_MASTERNODESUNDO)
            {
                // deleting them all, cause undo is not trivial
                BatchErase(key);
            }
            else
            {
                break;
            }
            pcursor->Next();
        }
    }
    {
        boost::scoped_ptr<CDBIterator> pcursor(const_cast<CDBWrapper*>(&*db)->NewIterator());
        pcursor->Seek(DB_SETOPERATORUNDO);

        while (pcursor->Valid())
        {
            boost::this_thread::interruption_point();
            std::pair<char, uint256> key;
            if (pcursor->GetKey(key) && key.first == DB_SETOPERATORUNDO)
            {
                if (operatorUndo.find(key.second) == operatorUndo.end())
                {
                    BatchErase(key);
                }
            }
            else
            {
                break;
            }
            pcursor->Next();
        }
    }
    {
        boost::scoped_ptr<CDBIterator> pcursor(const_cast<CDBWrapper*>(&*db)->NewIterator());
        pcursor->Seek(DB_TEAM);

        while (pcursor->Valid())
        {
            boost::this_thread::interruption_point();
            std::pair<std::pair<char, int32_t>, uint256> key;
            if (pcursor->GetKey(key) && key.first.first == DB_TEAM)
            {
                // deleting them all, cause teams may intersect
                /// @todo optimize
                BatchErase(key);
            }
            else
            {
                break;
            }
            pcursor->Next();
        }
    }

    // write all data
    for (auto && it = allNodes.begin(); it != allNodes.end(); ++it)
    {
        WriteMasternode(it->first, it->second);
    }
    for (auto && it = votes.begin(); it != votes.end(); ++it)
    {
        WriteVote(it->first, it->second);
    }
    for (auto && it = txsUndo.begin(); it != txsUndo.end(); ++it)
    {
        WriteUndo(it->first.first, it->first.second, it->second.first, static_cast<char>(it->second.second));
    }
    for (auto && it = operatorUndo.begin(); it != operatorUndo.end(); ++it)
    {
        WriteOperatorUndo(it->first, it->second);
    }
    for (auto && it = teams.begin(); it != teams.end(); ++it)
    {
        WriteTeam(it->first, it->second);
    }

    WriteHeight(lastHeight);

    CommitBatch();
    LogPrintf("MN: db saved: last height: %d; masternodes: %d; votes: %d; common undo: %d; operator undo: %d; teams: %d\n", lastHeight, allNodes.size(), votes.size(), txsUndo.size(), operatorUndo.size(), teams.size());

    return true;
}

CDposDB::CDposDB(size_t nCacheSize, bool fMemory, bool fWipe) :
    CDBWrapper(GetDataDir() / "dpos", nCacheSize, fMemory, fWipe)
{
}

void CDposDB::WriteViceBlock(const uint256& key, const CBlock& block, CDBBatch* batch)
{
    dbWrite(this, make_pair(DB_DPOS_VICE_BLOCKS, key), block, batch);
}

void CDposDB::WriteRoundVote(const uint256& key, const dpos::CRoundVote_p2p& vote, CDBBatch* batch)
{
    dbWrite(this, make_pair(DB_DPOS_ROUND_VOTES, key), vote, batch);
}

void CDposDB::WriteTxVote(const uint256& key, const dpos::CTxVote_p2p& vote, CDBBatch* batch)
{
    dbWrite(this, make_pair(DB_DPOS_TX_VOTES, key), vote, batch);
}

void CDposDB::EraseViceBlock(const uint256& key, CDBBatch* batch)
{
    dbErase(this, make_pair(DB_DPOS_VICE_BLOCKS, key), batch);
}

void CDposDB::EraseRoundVote(const uint256& key, CDBBatch* batch)
{
    dbErase(this, make_pair(DB_DPOS_ROUND_VOTES, key), batch);
}

void CDposDB::EraseTxVote(const uint256& key, CDBBatch* batch)
{
    dbErase(this, make_pair(DB_DPOS_TX_VOTES, key), batch);
}

bool CDposDB::LoadViceBlocks(std::function<void (const uint256&, const CBlock&)> onViceBlock)
{
    boost::scoped_ptr<CDBIterator> pcursor{NewIterator()};
    pcursor->Seek(DB_DPOS_VICE_BLOCKS);

    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        std::pair<char, uint256> key{};
        if (pcursor->GetKey(key) && key.first == DB_DPOS_VICE_BLOCKS) {
            CBlock block{};
            if (pcursor->GetValue(block)) {
                onViceBlock(key.second, block);
            } else {
                return error("CDposDB::LoadViceBlocks() : unable to read value");
            }
        } else {
            break;
        }
        pcursor->Next();
    }
    return true;
}

bool CDposDB::LoadRoundVotes(std::function<void (const uint256&, const dpos::CRoundVote_p2p&)> onRoundVote)
{
    boost::scoped_ptr<CDBIterator> pcursor{NewIterator()};
    pcursor->Seek(DB_DPOS_ROUND_VOTES);

    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        std::pair<char, uint256> key{};
        if (pcursor->GetKey(key) && key.first == DB_DPOS_ROUND_VOTES) {
            dpos::CRoundVote_p2p vote{};
            if (pcursor->GetValue(vote)) {
                onRoundVote(key.second, vote);
            } else {
                return error("CDposDB::LoadRoundVotes() : unable to read value");
            }
        } else {
            break;
        }
        pcursor->Next();
    }
    return true;
}

bool CDposDB::LoadTxVotes(std::function<void (const uint256&, const dpos::CTxVote_p2p&)> onTxVote)
{
    boost::scoped_ptr<CDBIterator> pcursor{NewIterator()};
    pcursor->Seek(DB_DPOS_TX_VOTES);

    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        std::pair<char, uint256> key{};
        if (pcursor->GetKey(key) && key.first == DB_DPOS_TX_VOTES) {
            dpos::CTxVote_p2p vote{};
            if (pcursor->GetValue(vote)) {
                onTxVote(key.second, vote);
            } else {
                return error("CDposDB::LoadTxVotes() : unable to read value");
            }
        } else {
            break;
        }
        pcursor->Next();
    }
    return true;
}
