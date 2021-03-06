// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin Core developers
// Copyright (c) 2018-2019 Crypticcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef BITCOIN_TXDB_H
#define BITCOIN_TXDB_H

#include "coins.h"
#include "dbwrapper.h"
//#include "masternodes/mntypes.h"
#include "chain.h"
#include "masternodes/masternodes.h"
#include "masternodes/dpos_p2p_messages.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <boost/function.hpp>

class CBlockFileInfo;
class CBlockIndex;

// START insightexplorer
struct CAddressUnspentKey;
struct CAddressUnspentValue;
struct CAddressIndexKey;
struct CAddressIndexIteratorKey;
struct CAddressIndexIteratorHeightKey;
struct CSpentIndexKey;
struct CSpentIndexValue;
struct CTimestampIndexKey;
struct CTimestampIndexIteratorKey;
struct CTimestampBlockIndexKey;
struct CTimestampBlockIndexValue;

typedef std::pair<CAddressUnspentKey, CAddressUnspentValue> CAddressUnspentDbEntry;
typedef std::pair<CAddressIndexKey, CAmount> CAddressIndexDbEntry;
typedef std::pair<CSpentIndexKey, CSpentIndexValue> CSpentIndexDbEntry;
// END insightexplorer


//! -dbcache default (MiB)
static const int64_t nDefaultDbCache = 450;
//! max. -dbcache (MiB)
static const int64_t nMaxDbCache = sizeof(void*) > 4 ? 16384 : 1024;
//! min. -dbcache in (MiB)
static const int64_t nMinDbCache = 4;

struct CDiskTxPos : public CDiskBlockPos
{
    unsigned int nTxOffset; // after header

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(*(CDiskBlockPos*)this);
        READWRITE(VARINT(nTxOffset));
    }

    CDiskTxPos(const CDiskBlockPos &blockIn, unsigned int nTxOffsetIn) : CDiskBlockPos(blockIn.nFile, blockIn.nPos), nTxOffset(nTxOffsetIn) {
    }

    CDiskTxPos() {
        SetNull();
    }

    void SetNull() {
        CDiskBlockPos::SetNull();
        nTxOffset = 0;
    }
};

/** CCoinsView backed by the coin database (chainstate/) */
class CCoinsViewDB : public CCoinsView
{
protected:
    CDBWrapper db;
    CCoinsViewDB(std::string dbName, size_t nCacheSize, bool fMemory = false, bool fWipe = false);
public:
    CCoinsViewDB(size_t nCacheSize, bool fMemory = false, bool fWipe = false);

    bool GetSproutAnchorAt(const uint256 &rt, SproutMerkleTree &tree) const;
    bool GetSaplingAnchorAt(const uint256 &rt, SaplingMerkleTree &tree) const;
    bool GetNullifier(const uint256 &nf, ShieldedType type) const;
    bool GetCoins(const uint256 &txid, CCoins &coins) const;
    bool HaveCoins(const uint256 &txid) const;
    uint256 GetBestBlock() const;
    uint256 GetBestAnchor(ShieldedType type) const;
    bool BatchWrite(CCoinsMap &mapCoins,
                    const uint256 &hashBlock,
                    const uint256 &hashSproutAnchor,
                    const uint256 &hashSaplingAnchor,
                    CAnchorsSproutMap &mapSproutAnchors,
                    CAnchorsSaplingMap &mapSaplingAnchors,
                    CNullifiersMap &mapSproutNullifiers,
                    CNullifiersMap &mapSaplingNullifiers);
    bool GetStats(CCoinsStats &stats) const;
};

/** Access to the block database (blocks/index/) */
class CBlockTreeDB : public CDBWrapper
{
public:
    CBlockTreeDB(size_t nCacheSize, bool fMemory = false, bool fWipe = false);
private:
    CBlockTreeDB(const CBlockTreeDB&);
    void operator=(const CBlockTreeDB&);
public:
    bool WriteBatchSync(const std::vector<std::pair<int, const CBlockFileInfo*> >& fileInfo, int nLastFile, const std::vector<const CBlockIndex*>& blockinfo);
    bool EraseBatchSync(const std::vector<const CBlockIndex*>& blockinfo);
    bool ReadBlockFileInfo(int nFile, CBlockFileInfo &fileinfo);
    bool ReadLastBlockFile(int &nFile);
    bool WriteReindexing(bool fReindex);
    bool ReadReindexing(bool &fReindex);
    bool ReadTxIndex(const uint256 &txid, CDiskTxPos &pos);
    bool WriteTxIndex(const std::vector<std::pair<uint256, CDiskTxPos> > &list);

    // START insightexplorer
    bool UpdateAddressUnspentIndex(const std::vector<CAddressUnspentDbEntry> &vect);
    bool ReadAddressUnspentIndex(uint160 addressHash, int type, std::vector<CAddressUnspentDbEntry> &vect);
    bool WriteAddressIndex(const std::vector<CAddressIndexDbEntry> &vect);
    bool EraseAddressIndex(const std::vector<CAddressIndexDbEntry> &vect);
    bool ReadAddressIndex(uint160 addressHash, int type, std::vector<CAddressIndexDbEntry> &addressIndex, int start = 0, int end = 0);
    bool ReadSpentIndex(CSpentIndexKey &key, CSpentIndexValue &value);
    bool UpdateSpentIndex(const std::vector<CSpentIndexDbEntry> &vect);
    bool WriteTimestampIndex(const CTimestampIndexKey &timestampIndex);
    bool ReadTimestampIndex(unsigned int high, unsigned int low,
            const bool fActiveOnly, std::vector<std::pair<uint256, unsigned int> > &vect);
    bool WriteTimestampBlockIndex(const CTimestampBlockIndexKey &blockhashIndex,
            const CTimestampBlockIndexValue &logicalts);
    bool ReadTimestampBlockIndex(const uint256 &hash, unsigned int &logicalTS);
    // END insightexplorer

    bool WriteFlag(const std::string &name, bool fValue);
    bool ReadFlag(const std::string &name, bool &fValue);
    bool LoadBlockIndexGuts(boost::function<CBlockIndex*(const uint256&)> insertBlockIndex);
};

/** Access to the masternodes database (masternodes/) */
class CMasternodesViewDB : public CMasternodesView
{
private:
    boost::shared_ptr<CDBWrapper> db;
    boost::scoped_ptr<CDBBatch> batch;

public:
    CMasternodesViewDB(size_t nCacheSize, bool fMemory = false, bool fWipe = false);
    ~CMasternodesViewDB() override {}

protected:
    // for test purposes only
    CMasternodesViewDB();

private:
    CMasternodesViewDB(CMasternodesViewDB const & other) = delete;
    CMasternodesViewDB & operator=(CMasternodesViewDB const &) = delete;

    template <typename K, typename V>
    void BatchWrite(const K& key, const V& value)
    {
        if (!batch)
        {
            batch.reset(new CDBBatch(*db));
        }
        batch->Write<K,V>(key, value);
    }
    template <typename K>
    void BatchErase(const K& key)
    {
        if (!batch)
        {
            batch.reset(new CDBBatch(*db));
        }
        batch->Erase<K>(key);
    }

protected:
    void CommitBatch();
    void DropBatch();

    bool ReadHeight(int & h);
    void WriteHeight(int h);

    void WriteMasternode(uint256 const & txid, CMasternode const & node);
    void EraseMasternode(uint256 const & txid);

    void WriteVote(uint256 const & txid, CDismissVote const & vote);
    void EraseVote(uint256 const & txid);

    void WriteDeadIndex(int height, uint256 const & txid, char type);
    void EraseDeadIndex(int height, uint256 const & txid);

    void WriteUndo(int height, uint256 const & txid, uint256 const & affectedNode, char undoType);
    void EraseUndo(int height, uint256 const & txid, uint256 const & affectedItem);

    void ReadOperatorUndo(uint256 const & txid, COperatorUndoRec & value);
    void WriteOperatorUndo(uint256 const & txid, COperatorUndoRec const & value);
    void EraseOperatorUndo(uint256 const & txid);

    void WriteTeam(int blockHeight, CTeam const & team);

public:
    bool Load() override;
    bool Flush() override;

private:
    bool LoadMasternodes(std::function<void(uint256 &, CMasternode &)> onNode) const;
    bool LoadVotes(std::function<void(uint256 const &, CDismissVote const &)> onVote) const;
    bool LoadUndo(std::function<void(int, uint256 const &, uint256 const &, char)> onUndo) const;
    bool LoadTeams(CTeams & teams) const;
};


/** Access to the dPoS votes and blocks database (dpos/) */
class CDposDB : public CDBWrapper
{
public:
    CDposDB(size_t nCacheSize, bool fMemory = false, bool fWipe = false);
    CDposDB(const CDposDB&) = delete;
    CDposDB& operator=(const CDposDB&) = delete;

public:
    void WriteViceBlock(const uint256& key, const CBlock& block, CDBBatch* batch = nullptr);
    void WriteRoundVote(const uint256& key, const dpos::CRoundVote_p2p& vote, CDBBatch* batch = nullptr);
    void WriteTxVote(const uint256& key, const dpos::CTxVote_p2p& vote, CDBBatch* batch = nullptr);

    void EraseViceBlock(const uint256& key, CDBBatch* batch = nullptr);
    void EraseRoundVote(const uint256& key, CDBBatch* batch = nullptr);
    void EraseTxVote(const uint256& key, CDBBatch* batch = nullptr);

    bool LoadViceBlocks(std::function<void(const uint256&, const CBlock&)> onViceBlock);
    bool LoadRoundVotes(std::function<void(const uint256&, const dpos::CRoundVote_p2p&)> onRoundVote);
    bool LoadTxVotes(std::function<void(const uint256&, const dpos::CTxVote_p2p&)> onTxVote);
};

#endif // BITCOIN_TXDB_H
