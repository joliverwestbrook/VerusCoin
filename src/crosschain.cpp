#include "cc/eval.h"
#include "crosschain.h"
#include "importcoin.h"
#include "main.h"
#include "notarisationdb.h"


int NOTARISATION_SCAN_LIMIT_BLOCKS = 1440;


/*
 * This file is built in the server
 */

/* On KMD */
uint256 CalculateProofRoot(const char* symbol, uint32_t targetCCid, int kmdHeight,
        std::vector<uint256> &moms, uint256 &destNotarisationTxid)
{
    /*
     * Notaries don't wait for confirmation on KMD before performing a backnotarisation,
     * but we need a determinable range that will encompass all merkle roots. Include MoMs
     * including the block height of the last notarisation until the height before the
     * previous notarisation.
     *
     *    kmdHeight      notarisations-0      notarisations-1
     *        |                |********************|
     *        > scan backwards >
     */

    if (targetCCid <= 1)
        return uint256();

    if (kmdHeight < 0 || kmdHeight > chainActive.Height())
        return uint256();

    int seenOwnNotarisations = 0;

    for (int i=0; i<NOTARISATION_SCAN_LIMIT_BLOCKS; i++) {
        if (i > kmdHeight) break;
        NotarisationsInBlock notarisations;
        uint256 blockHash = *chainActive[kmdHeight-i]->phashBlock;
        if (!GetBlockNotarisations(blockHash, notarisations))
            continue;
        BOOST_FOREACH(Notarisation& nota, notarisations) {
            NotarisationData& data = nota.second;
            if (data.ccId != targetCCid)
                continue;
            if (strcmp(data.symbol, symbol) == 0)
            {
                seenOwnNotarisations++;
                if (seenOwnNotarisations == 2)
                    goto end;
                if (seenOwnNotarisations == 1)
                    destNotarisationTxid = nota.first;
            }
            if (seenOwnNotarisations == 1)
                moms.push_back(data.MoM);
        }
    }

end:
    return GetMerkleRoot(moms);
}


/*
 * Get a notarisation from a given height
 *
 * Will scan notarisations leveldb up to a limit
 */
template <typename IsTarget>
int ScanNotarisationsFromHeight(int nHeight, const IsTarget f, Notarisation &found)
{
    int limit = std::min(nHeight + NOTARISATION_SCAN_LIMIT_BLOCKS, chainActive.Height());
    
    for (int h=nHeight; h<limit; h++) {
        NotarisationsInBlock notarisations;

        if (!GetBlockNotarisations(*chainActive[h]->phashBlock, notarisations))
            continue;

        BOOST_FOREACH(found, notarisations) {
            if (f(found)) {
                return h;
            }
        }
    }
    return 0;
}


/* On KMD */
TxProof GetCrossChainProof(const uint256 txid, const char* targetSymbol, uint32_t targetCCid,
        const TxProof assetChainProof)
{
    /*
     * Here we are given a proof generated by an assetchain A which goes from given txid to
     * an assetchain MoM. We need to go from the notarisationTxid for A to the MoMoM range of the
     * backnotarisation for B (given by kmdheight of notarisation), find the MoM within the MoMs for
     * that range, and finally extend the proof to lead to the MoMoM (proof root).
     */
    EvalRef eval;
    uint256 MoM = assetChainProof.second.Exec(txid);
    
    // Get a kmd height for given notarisation Txid
    int kmdHeight;
    {
        CTransaction sourceNotarisation;
        uint256 hashBlock;
        CBlockIndex blockIdx;
        if (eval->GetTxConfirmed(assetChainProof.first, sourceNotarisation, blockIdx))
            kmdHeight = blockIdx.nHeight;
        else if (eval->GetTxUnconfirmed(assetChainProof.first, sourceNotarisation, hashBlock))
            kmdHeight = chainActive.Tip()->nHeight;
        else
            throw std::runtime_error("Notarisation not found");
    }

    // Get MoMs for kmd height and symbol
    std::vector<uint256> moms;
    uint256 targetChainNotarisationTxid;
    uint256 MoMoM = CalculateProofRoot(targetSymbol, targetCCid, kmdHeight, moms, targetChainNotarisationTxid);
    if (MoMoM.IsNull())
        throw std::runtime_error("No MoMs found");
    
    // Find index of source MoM in MoMoM
    int nIndex;
    for (nIndex=0; nIndex<moms.size(); nIndex++) {
        if (moms[nIndex] == MoM)
            goto cont;
    }
    throw std::runtime_error("Couldn't find MoM within MoMoM set");
cont:

    // Create a branch
    std::vector<uint256> vBranch;
    {
        CBlock fakeBlock;
        for (int i=0; i<moms.size(); i++) {
            CTransaction fakeTx;
            // first value in CTransaction memory is it's hash
            memcpy((void*)&fakeTx, moms[i].begin(), 32);
            fakeBlock.vtx.push_back(fakeTx);
        }
        vBranch = fakeBlock.GetMerkleBranch(nIndex);
    }

    // Concatenate branches
    MerkleBranch newBranch = assetChainProof.second;
    newBranch << MerkleBranch(nIndex, vBranch);

    // Check proof
    if (newBranch.Exec(txid) != MoMoM)
        throw std::runtime_error("Proof check failed");

    return std::make_pair(targetChainNotarisationTxid,newBranch);
}


/*
 * Takes an importTx that has proof leading to assetchain root
 * and extends proof to cross chain root
 */
void CompleteImportTransaction(CTransaction &importTx)
{
    TxProof proof;
    CTransaction burnTx;
    std::vector<CTxOut> payouts;
    if (!UnmarshalImportTx(importTx, proof, burnTx, payouts))
        throw std::runtime_error("Couldn't parse importTx");

    std::string targetSymbol;
    uint32_t targetCCid;
    uint256 payoutsHash;
    if (!UnmarshalBurnTx(burnTx, targetSymbol, &targetCCid, payoutsHash))
        throw std::runtime_error("Couldn't parse burnTx");

    proof = GetCrossChainProof(burnTx.GetHash(), targetSymbol.data(), targetCCid, proof);

    importTx = MakeImportCoinTransaction(proof, burnTx, payouts);
}


bool IsSameAssetChain(const Notarisation &nota) {
    return strcmp(nota.second.symbol, ASSETCHAINS_SYMBOL) == 0;
};


/* On assetchain */
bool GetNextBacknotarisation(uint256 kmdNotarisationTxid, Notarisation &out)
{
    /*
     * Here we are given a txid, and a proof.
     * We go from the KMD notarisation txid to the backnotarisation,
     * then jump to the next backnotarisation, which contains the corresponding MoMoM.
     */
    Notarisation bn;
    if (!GetBackNotarisation(kmdNotarisationTxid, bn))
        return false;

    return (bool) ScanNotarisationsFromHeight(bn.second.height+1, &IsSameAssetChain, out);
}


/*
 * On assetchain
 * in: txid
 * out: pair<notarisationTxHash,merkleBranch>
 */
TxProof GetAssetchainProof(uint256 hash)
{
    int nIndex;
    CBlockIndex* blockIndex;
    Notarisation nota;
    std::vector<uint256> branch;

    {
        uint256 blockHash;
        CTransaction tx;
        if (!GetTransaction(hash, tx, blockHash, true))
            throw std::runtime_error("cannot find transaction");

        if (blockHash.IsNull())
            throw std::runtime_error("tx still in mempool");

        blockIndex = mapBlockIndex[blockHash];
        if (!ScanNotarisationsFromHeight(blockIndex->nHeight, &IsSameAssetChain, nota))
            throw std::runtime_error("notarisation not found");
        
        // index of block in MoM leaves
        nIndex = nota.second.height - blockIndex->nHeight;
    }

    // build merkle chain from blocks to MoM
    {
        std::vector<uint256> leaves, tree;
        for (int i=0; i<nota.second.MoMDepth; i++) {
            uint256 mRoot = chainActive[nota.second.height - i]->hashMerkleRoot;
            leaves.push_back(mRoot);
        }
        bool fMutated;
        BuildMerkleTree(&fMutated, leaves, tree);
        branch = GetMerkleBranch(nIndex, leaves.size(), tree); 

        // Check branch
        uint256 ourResult = SafeCheckMerkleBranch(blockIndex->hashMerkleRoot, branch, nIndex);
        if (nota.second.MoM != ourResult)
            throw std::runtime_error("Failed merkle block->MoM");
    }

    // Now get the tx merkle branch
    {
        CBlock block;

        if (fHavePruned && !(blockIndex->nStatus & BLOCK_HAVE_DATA) && blockIndex->nTx > 0)
            throw std::runtime_error("Block not available (pruned data)");

        if(!ReadBlockFromDisk(block, blockIndex,1))
            throw std::runtime_error("Can't read block from disk");

        // Locate the transaction in the block
        int nTxIndex;
        for (nTxIndex = 0; nTxIndex < (int)block.vtx.size(); nTxIndex++)
            if (block.vtx[nTxIndex].GetHash() == hash)
                break;

        if (nTxIndex == (int)block.vtx.size())
            throw std::runtime_error("Error locating tx in block");

        std::vector<uint256> txBranch = block.GetMerkleBranch(nTxIndex);

        // Check branch
        if (block.hashMerkleRoot != CBlock::CheckMerkleBranch(hash, txBranch, nTxIndex))
            throw std::runtime_error("Failed merkle tx->block");

        // concatenate branches
        nIndex = (nIndex << txBranch.size()) + nTxIndex;
        branch.insert(branch.begin(), txBranch.begin(), txBranch.end());
    }

    // Check the proof
    if (nota.second.MoM != CBlock::CheckMerkleBranch(hash, branch, nIndex)) 
        throw std::runtime_error("Failed validating MoM");

    // All done!
    CDataStream ssProof(SER_NETWORK, PROTOCOL_VERSION);
    return std::make_pair(nota.second.txHash, MerkleBranch(nIndex, branch));
}
