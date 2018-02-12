#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <sodium.h>

#include "main.h"
#include "primitives/transaction.h"
#include "consensus/validation.h"

TEST(checktransaction_tests, check_vpub_not_both_nonzero) {
    CMutableTransaction tx;
    tx.nVersion = 2;

    {
        // Ensure that values within the joinsplit are well-formed.
        CMutableTransaction newTx(tx);
        CValidationState state;

        newTx.vjoinsplit.push_back(JSDescription());

        JSDescription *jsdesc = &newTx.vjoinsplit[0];
        jsdesc->vpub_old = 1;
        jsdesc->vpub_new = 1;

        EXPECT_FALSE(CheckTransactionWithoutProofVerification(newTx, state));
        EXPECT_EQ(state.GetRejectReason(), "bad-txns-vpubs-both-nonzero");
    }
}

class MockCValidationState : public CValidationState {
public:
    MOCK_METHOD5(DoS, bool(int level, bool ret,
             unsigned char chRejectCodeIn, std::string strRejectReasonIn,
             bool corruptionIn));
    MOCK_METHOD3(Invalid, bool(bool ret,
                 unsigned char _chRejectCode, std::string _strRejectReason));
    MOCK_METHOD1(Error, bool(std::string strRejectReasonIn));
    MOCK_CONST_METHOD0(IsValid, bool());
    MOCK_CONST_METHOD0(IsInvalid, bool());
    MOCK_CONST_METHOD0(IsError, bool());
    MOCK_CONST_METHOD1(IsInvalid, bool(int &nDoSOut));
    MOCK_CONST_METHOD0(CorruptionPossible, bool());
    MOCK_CONST_METHOD0(GetRejectCode, unsigned char());
    MOCK_CONST_METHOD0(GetRejectReason, std::string());
};


CMutableTransaction GetValidTransaction() {
    CMutableTransaction mtx;
    mtx.vin.resize(2);
    mtx.vin[0].prevout.hash = uint256S("0000000000000000000000000000000000000000000000000000000000000001");
    mtx.vin[0].prevout.n = 0;
    mtx.vin[1].prevout.hash = uint256S("0000000000000000000000000000000000000000000000000000000000000002");
    mtx.vin[1].prevout.n = 0;
    mtx.vout.resize(2);
    // mtx.vout[0].scriptPubKey = 
    mtx.vout[0].nValue = 0;
    mtx.vout[1].nValue = 0;
    mtx.vjoinsplit.resize(2);
    mtx.vjoinsplit[0].nullifiers.at(0) = uint256S("0000000000000000000000000000000000000000000000000000000000000000");
    mtx.vjoinsplit[0].nullifiers.at(1) = uint256S("0000000000000000000000000000000000000000000000000000000000000001");
    mtx.vjoinsplit[1].nullifiers.at(0) = uint256S("0000000000000000000000000000000000000000000000000000000000000002");
    mtx.vjoinsplit[1].nullifiers.at(1) = uint256S("0000000000000000000000000000000000000000000000000000000000000003");


    // Generate an ephemeral keypair.
    uint256 joinSplitPubKey;
    unsigned char joinSplitPrivKey[crypto_sign_SECRETKEYBYTES];
    crypto_sign_keypair(joinSplitPubKey.begin(), joinSplitPrivKey);
    mtx.joinSplitPubKey = joinSplitPubKey;

    // Compute the correct hSig.
    // TODO: #966.
    static const uint256 one(uint256S("0000000000000000000000000000000000000000000000000000000000000001"));
    // Empty output script.
    CScript scriptCode;
    CTransaction signTx(mtx);
    uint256 dataToBeSigned = SignatureHash(scriptCode, signTx, NOT_AN_INPUT, SIGHASH_ALL);
    if (dataToBeSigned == one) {
        throw std::runtime_error("SignatureHash failed");
    }

    // Add the signature
    assert(crypto_sign_detached(&mtx.joinSplitSig[0], NULL,
                         dataToBeSigned.begin(), 32,
                         joinSplitPrivKey
                        ) == 0);
    return mtx;
}

TEST(checktransaction_tests, valid_transaction) {
    CMutableTransaction mtx = GetValidTransaction();
    CTransaction tx(mtx);
    MockCValidationState state;
    EXPECT_TRUE(CheckTransactionWithoutProofVerification(tx, state));
}

TEST(checktransaction_tests, BadVersionTooLow) {
    CMutableTransaction mtx = GetValidTransaction();
    mtx.nVersion = 0;

    CTransaction tx(mtx);
    MockCValidationState state;
    EXPECT_CALL(state, DoS(100, false, REJECT_INVALID, "bad-txns-version-too-low", false)).Times(1);
    CheckTransactionWithoutProofVerification(tx, state);
}

TEST(checktransaction_tests, bad_txns_vin_empty) {
    CMutableTransaction mtx = GetValidTransaction();
    mtx.vjoinsplit.resize(0);
    mtx.vin.resize(0);

    CTransaction tx(mtx);
    MockCValidationState state;
    EXPECT_CALL(state, DoS(10, false, REJECT_INVALID, "bad-txns-vin-empty", false)).Times(1);
    CheckTransactionWithoutProofVerification(tx, state);
}

TEST(checktransaction_tests, bad_txns_vout_empty) {
    CMutableTransaction mtx = GetValidTransaction();
    mtx.vjoinsplit.resize(0);
    mtx.vout.resize(0);

    CTransaction tx(mtx);

    MockCValidationState state;
    EXPECT_CALL(state, DoS(10, false, REJECT_INVALID, "bad-txns-vout-empty", false)).Times(1);
    CheckTransactionWithoutProofVerification(tx, state);
}

TEST(checktransaction_tests, bad_txns_oversize) {
    CMutableTransaction mtx = GetValidTransaction();

    mtx.vin[0].scriptSig = CScript();
    std::vector<unsigned char> vchData(520);
    for (unsigned int i = 0; i < 190; ++i)
        mtx.vin[0].scriptSig << vchData << OP_DROP;
    mtx.vin[0].scriptSig << OP_1;

    {
        // Transaction is just under the limit...
        CTransaction tx(mtx);
        CValidationState state;
        ASSERT_TRUE(CheckTransactionWithoutProofVerification(tx, state));
    }

    // Not anymore!
    mtx.vin[1].scriptSig << vchData << OP_DROP;
    mtx.vin[1].scriptSig << OP_1;

    {
        CTransaction tx(mtx);
        ASSERT_EQ(::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION), 100202);

        MockCValidationState state;
        EXPECT_CALL(state, DoS(100, false, REJECT_INVALID, "bad-txns-oversize", false)).Times(1);
        CheckTransactionWithoutProofVerification(tx, state);
    }
}

TEST(checktransaction_tests, bad_txns_vout_negative) {
    CMutableTransaction mtx = GetValidTransaction();
    mtx.vout[0].nValue = -1;

    CTransaction tx(mtx);

    MockCValidationState state;
    EXPECT_CALL(state, DoS(100, false, REJECT_INVALID, "bad-txns-vout-negative", false)).Times(1);
    CheckTransactionWithoutProofVerification(tx, state);
}

TEST(checktransaction_tests, bad_txns_vout_toolarge) {
    CMutableTransaction mtx = GetValidTransaction();
    mtx.vout[0].nValue = MAX_MONEY + 1;

    CTransaction tx(mtx);

    MockCValidationState state;
    EXPECT_CALL(state, DoS(100, false, REJECT_INVALID, "bad-txns-vout-toolarge", false)).Times(1);
    CheckTransactionWithoutProofVerification(tx, state);
}

TEST(checktransaction_tests, bad_txns_txouttotal_toolarge_outputs) {
    CMutableTransaction mtx = GetValidTransaction();
    mtx.vout[0].nValue = MAX_MONEY;
    mtx.vout[1].nValue = 1;

    CTransaction tx(mtx);

    MockCValidationState state;
    EXPECT_CALL(state, DoS(100, false, REJECT_INVALID, "bad-txns-txouttotal-toolarge", false)).Times(1);
    CheckTransactionWithoutProofVerification(tx, state);
}

TEST(checktransaction_tests, bad_txns_txouttotal_toolarge_joinsplit) {
    CMutableTransaction mtx = GetValidTransaction();
    mtx.vout[0].nValue = 1;
    mtx.vjoinsplit[0].vpub_old = MAX_MONEY;

    CTransaction tx(mtx);

    MockCValidationState state;
    EXPECT_CALL(state, DoS(100, false, REJECT_INVALID, "bad-txns-txouttotal-toolarge", false)).Times(1);
    CheckTransactionWithoutProofVerification(tx, state);
}

TEST(checktransaction_tests, bad_txns_txintotal_toolarge_joinsplit) {
    CMutableTransaction mtx = GetValidTransaction();
    mtx.vjoinsplit[0].vpub_new = MAX_MONEY - 1;
    mtx.vjoinsplit[1].vpub_new = MAX_MONEY - 1;

    CTransaction tx(mtx);

    MockCValidationState state;
    EXPECT_CALL(state, DoS(100, false, REJECT_INVALID, "bad-txns-txintotal-toolarge", false)).Times(1);
    CheckTransactionWithoutProofVerification(tx, state);
}

TEST(checktransaction_tests, bad_txns_vpub_old_negative) {
    CMutableTransaction mtx = GetValidTransaction();
    mtx.vjoinsplit[0].vpub_old = -1;

    CTransaction tx(mtx);

    MockCValidationState state;
    EXPECT_CALL(state, DoS(100, false, REJECT_INVALID, "bad-txns-vpub_old-negative", false)).Times(1);
    CheckTransactionWithoutProofVerification(tx, state);
}

TEST(checktransaction_tests, bad_txns_vpub_new_negative) {
    CMutableTransaction mtx = GetValidTransaction();
    mtx.vjoinsplit[0].vpub_new = -1;

    CTransaction tx(mtx);

    MockCValidationState state;
    EXPECT_CALL(state, DoS(100, false, REJECT_INVALID, "bad-txns-vpub_new-negative", false)).Times(1);
    CheckTransactionWithoutProofVerification(tx, state);
}

TEST(checktransaction_tests, bad_txns_vpub_old_toolarge) {
    CMutableTransaction mtx = GetValidTransaction();
    mtx.vjoinsplit[0].vpub_old = MAX_MONEY + 1;

    CTransaction tx(mtx);

    MockCValidationState state;
    EXPECT_CALL(state, DoS(100, false, REJECT_INVALID, "bad-txns-vpub_old-toolarge", false)).Times(1);
    CheckTransactionWithoutProofVerification(tx, state);
}

TEST(checktransaction_tests, bad_txns_vpub_new_toolarge) {
    CMutableTransaction mtx = GetValidTransaction();
    mtx.vjoinsplit[0].vpub_new = MAX_MONEY + 1;

    CTransaction tx(mtx);

    MockCValidationState state;
    EXPECT_CALL(state, DoS(100, false, REJECT_INVALID, "bad-txns-vpub_new-toolarge", false)).Times(1);
    CheckTransactionWithoutProofVerification(tx, state);
}

TEST(checktransaction_tests, bad_txns_vpubs_both_nonzero) {
    CMutableTransaction mtx = GetValidTransaction();
    mtx.vjoinsplit[0].vpub_old = 1;
    mtx.vjoinsplit[0].vpub_new = 1;

    CTransaction tx(mtx);

    MockCValidationState state;
    EXPECT_CALL(state, DoS(100, false, REJECT_INVALID, "bad-txns-vpubs-both-nonzero", false)).Times(1);
    CheckTransactionWithoutProofVerification(tx, state);
}

TEST(checktransaction_tests, bad_txns_inputs_duplicate) {
    CMutableTransaction mtx = GetValidTransaction();
    mtx.vin[1].prevout.hash = mtx.vin[0].prevout.hash;
    mtx.vin[1].prevout.n = mtx.vin[0].prevout.n;

    CTransaction tx(mtx);

    MockCValidationState state;
    EXPECT_CALL(state, DoS(100, false, REJECT_INVALID, "bad-txns-inputs-duplicate", false)).Times(1);
    CheckTransactionWithoutProofVerification(tx, state);
}

TEST(checktransaction_tests, bad_joinsplits_nullifiers_duplicate_same_joinsplit) {
    CMutableTransaction mtx = GetValidTransaction();
    mtx.vjoinsplit[0].nullifiers.at(0) = uint256S("0000000000000000000000000000000000000000000000000000000000000000");
    mtx.vjoinsplit[0].nullifiers.at(1) = uint256S("0000000000000000000000000000000000000000000000000000000000000000");

    CTransaction tx(mtx);

    MockCValidationState state;
    EXPECT_CALL(state, DoS(100, false, REJECT_INVALID, "bad-joinsplits-nullifiers-duplicate", false)).Times(1);
    CheckTransactionWithoutProofVerification(tx, state);
}

TEST(checktransaction_tests, bad_joinsplits_nullifiers_duplicate_different_joinsplit) {
    CMutableTransaction mtx = GetValidTransaction();
    mtx.vjoinsplit[0].nullifiers.at(0) = uint256S("0000000000000000000000000000000000000000000000000000000000000000");
    mtx.vjoinsplit[1].nullifiers.at(0) = uint256S("0000000000000000000000000000000000000000000000000000000000000000");

    CTransaction tx(mtx);

    MockCValidationState state;
    EXPECT_CALL(state, DoS(100, false, REJECT_INVALID, "bad-joinsplits-nullifiers-duplicate", false)).Times(1);
    CheckTransactionWithoutProofVerification(tx, state);
}

TEST(checktransaction_tests, bad_cb_has_joinsplits) {
    CMutableTransaction mtx = GetValidTransaction();
    // Make it a coinbase.
    mtx.vin.resize(1);
    mtx.vin[0].prevout.SetNull();

    mtx.vjoinsplit.resize(1);

    CTransaction tx(mtx);
    EXPECT_TRUE(tx.IsCoinBase());

    MockCValidationState state;
    EXPECT_CALL(state, DoS(100, false, REJECT_INVALID, "bad-cb-has-joinsplits", false)).Times(1);
    CheckTransactionWithoutProofVerification(tx, state);
}

TEST(checktransaction_tests, bad_cb_empty_scriptsig) {
    CMutableTransaction mtx = GetValidTransaction();
    // Make it a coinbase.
    mtx.vin.resize(1);
    mtx.vin[0].prevout.SetNull();

    mtx.vjoinsplit.resize(0);

    CTransaction tx(mtx);
    EXPECT_TRUE(tx.IsCoinBase());

    MockCValidationState state;
    EXPECT_CALL(state, DoS(100, false, REJECT_INVALID, "bad-cb-length", false)).Times(1);
    CheckTransactionWithoutProofVerification(tx, state);
}

TEST(checktransaction_tests, bad_txns_prevout_null) {
    CMutableTransaction mtx = GetValidTransaction();
    mtx.vin[1].prevout.SetNull();

    CTransaction tx(mtx);
    EXPECT_FALSE(tx.IsCoinBase());

    MockCValidationState state;
    EXPECT_CALL(state, DoS(10, false, REJECT_INVALID, "bad-txns-prevout-null", false)).Times(1);
    CheckTransactionWithoutProofVerification(tx, state);
}

TEST(checktransaction_tests, bad_txns_invalid_joinsplit_signature) {
    CMutableTransaction mtx = GetValidTransaction();
    mtx.joinSplitSig[0] += 1;
    CTransaction tx(mtx);

    MockCValidationState state;
    EXPECT_CALL(state, DoS(100, false, REJECT_INVALID, "bad-txns-invalid-joinsplit-signature", false)).Times(1);
    CheckTransactionWithoutProofVerification(tx, state);
}

TEST(checktransaction_tests, non_canonical_ed25519_signature) {
    CMutableTransaction mtx = GetValidTransaction();

    // Check that the signature is valid before we add L
    {
        CTransaction tx(mtx);
        MockCValidationState state;
        EXPECT_TRUE(CheckTransactionWithoutProofVerification(tx, state));
    }

    // Copied from libsodium/crypto_sign/ed25519/ref10/open.c
    static const unsigned char L[32] =
      { 0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
        0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10 };

    // Add L to S, which starts at mtx.joinSplitSig[32].
    unsigned int s = 0;
    for (size_t i = 0; i < 32; i++) {
        s = mtx.joinSplitSig[32 + i] + L[i] + (s >> 8);
        mtx.joinSplitSig[32 + i] = s & 0xff;
    }

    CTransaction tx(mtx);

    MockCValidationState state;
    EXPECT_CALL(state, DoS(100, false, REJECT_INVALID, "bad-txns-invalid-joinsplit-signature", false)).Times(1);
    CheckTransactionWithoutProofVerification(tx, state);
}

TEST(checktransaction_tests, OverwinterConstructors) {
    CMutableTransaction mtx;
    mtx.fOverwintered = true;
    mtx.nVersion = 3;
    mtx.nVersionGroupId = OVERWINTER_VERSION_GROUP_ID;
    mtx.nExpiryHeight = 20;

    // Check constructor with overwinter fields
    CTransaction tx(mtx);
    EXPECT_EQ(tx.nVersion, mtx.nVersion);
    EXPECT_EQ(tx.fOverwintered, mtx.fOverwintered);
    EXPECT_EQ(tx.nVersionGroupId, mtx.nVersionGroupId);
    EXPECT_EQ(tx.nExpiryHeight, mtx.nExpiryHeight);

    // Check constructor of mutable transaction struct
    CMutableTransaction mtx2(tx);
    EXPECT_EQ(mtx2.nVersion, mtx.nVersion);
    EXPECT_EQ(mtx2.fOverwintered, mtx.fOverwintered);
    EXPECT_EQ(mtx2.nVersionGroupId, mtx.nVersionGroupId);
    EXPECT_EQ(mtx2.nExpiryHeight, mtx.nExpiryHeight);
    EXPECT_TRUE(mtx2.GetHash() == mtx.GetHash());

    // Check assignment of overwinter fields
    CTransaction tx2 = tx;
    EXPECT_EQ(tx2.nVersion, mtx.nVersion);
    EXPECT_EQ(tx2.fOverwintered, mtx.fOverwintered);
    EXPECT_EQ(tx2.nVersionGroupId, mtx.nVersionGroupId);
    EXPECT_EQ(tx2.nExpiryHeight, mtx.nExpiryHeight);
    EXPECT_TRUE(tx2 == tx);
}

TEST(checktransaction_tests, OverwinterSerialization) {
    CMutableTransaction mtx;
    mtx.fOverwintered = true;
    mtx.nVersion = 3;
    mtx.nVersionGroupId = OVERWINTER_VERSION_GROUP_ID;
    mtx.nExpiryHeight = 99;

    // Check round-trip serialization and deserializtion from mtx to tx.
    {
        CDataStream ss(SER_DISK, PROTOCOL_VERSION);
        ss << mtx;
        CTransaction tx;
        ss >> tx;
        EXPECT_EQ(mtx.nVersion, tx.nVersion);
        EXPECT_EQ(mtx.fOverwintered, tx.fOverwintered);
        EXPECT_EQ(mtx.nVersionGroupId, tx.nVersionGroupId);
        EXPECT_EQ(mtx.nExpiryHeight, tx.nExpiryHeight);

        EXPECT_EQ(mtx.GetHash(), CMutableTransaction(tx).GetHash());
        EXPECT_EQ(tx.GetHash(), CTransaction(mtx).GetHash());
    }

    // Also check mtx to mtx
    {
        CDataStream ss(SER_DISK, PROTOCOL_VERSION);
        ss << mtx;
        CMutableTransaction mtx2;
        ss >> mtx2;
        EXPECT_EQ(mtx.nVersion, mtx2.nVersion);
        EXPECT_EQ(mtx.fOverwintered, mtx2.fOverwintered);
        EXPECT_EQ(mtx.nVersionGroupId, mtx2.nVersionGroupId);
        EXPECT_EQ(mtx.nExpiryHeight, mtx2.nExpiryHeight);

        EXPECT_EQ(mtx.GetHash(), mtx2.GetHash());
    }

    // Alco check tx to tx
    {
        CTransaction tx(mtx);
        CDataStream ss(SER_DISK, PROTOCOL_VERSION);
        ss << tx;
        CTransaction tx2;
        ss >> tx2;
        EXPECT_EQ(tx.nVersion, tx2.nVersion);
        EXPECT_EQ(tx.fOverwintered, tx2.fOverwintered);
        EXPECT_EQ(tx.nVersionGroupId, tx2.nVersionGroupId);
        EXPECT_EQ(tx.nExpiryHeight, tx2.nExpiryHeight);

        EXPECT_EQ(mtx.GetHash(), CMutableTransaction(tx).GetHash());
        EXPECT_EQ(tx.GetHash(), tx2.GetHash());
    }
}

TEST(checktransaction_tests, OverwinterDefaultValues) {
    // Check default values (this will fail when defaults change; test should then be updated)
    CTransaction tx;
    EXPECT_EQ(tx.nVersion, 1);
    EXPECT_EQ(tx.fOverwintered, false);
    EXPECT_EQ(tx.nVersionGroupId, 0);
    EXPECT_EQ(tx.nExpiryHeight, 0);
}

// A valid v3 transaction with no joinsplits
TEST(checktransaction_tests, OverwinterValidTx) {
    CMutableTransaction mtx = GetValidTransaction();
    mtx.vjoinsplit.resize(0);
    mtx.fOverwintered = true;
    mtx.nVersion = 3;
    mtx.nVersionGroupId = OVERWINTER_VERSION_GROUP_ID;
    mtx.nExpiryHeight = 0;
    CTransaction tx(mtx);
    MockCValidationState state;
    EXPECT_TRUE(CheckTransactionWithoutProofVerification(tx, state));
}

TEST(checktransaction_tests, OverwinterExpiryHeight) {
    CMutableTransaction mtx = GetValidTransaction();
    mtx.vjoinsplit.resize(0);
    mtx.fOverwintered = true;
    mtx.nVersion = 3;
    mtx.nVersionGroupId = OVERWINTER_VERSION_GROUP_ID;
    mtx.nExpiryHeight = 0;

    {
        CTransaction tx(mtx);
        MockCValidationState state;
        EXPECT_TRUE(CheckTransactionWithoutProofVerification(tx, state));
    }

    {
        mtx.nExpiryHeight = TX_EXPIRY_HEIGHT_THRESHOLD;
        CTransaction tx(mtx);
        MockCValidationState state;
        EXPECT_TRUE(CheckTransactionWithoutProofVerification(tx, state));
    }

    {
        mtx.nExpiryHeight = TX_EXPIRY_HEIGHT_THRESHOLD + 1;
        CTransaction tx(mtx);
        MockCValidationState state;
        EXPECT_CALL(state, DoS(100, false, REJECT_INVALID, "bad-tx-expiry-height-too-high", false)).Times(1);
        CheckTransactionWithoutProofVerification(tx, state);
    }

    {
        mtx.nExpiryHeight = std::numeric_limits<uint32_t>::max();
        CTransaction tx(mtx);
        MockCValidationState state;
        EXPECT_CALL(state, DoS(100, false, REJECT_INVALID, "bad-tx-expiry-height-too-high", false)).Times(1);
        CheckTransactionWithoutProofVerification(tx, state);
    }
}


// Test that a sprout tx with a negative version number is detected
// given the new Overwinter logic
TEST(checktransaction_tests, SproutTxVersionTooLow) {
    CMutableTransaction mtx = GetValidTransaction();
    mtx.vjoinsplit.resize(0);
    mtx.fOverwintered = false;
    mtx.nVersion = -1;

    CTransaction tx(mtx);
    MockCValidationState state;
    EXPECT_CALL(state, DoS(100, false, REJECT_INVALID, "bad-txns-version-too-low", false)).Times(1);
    CheckTransactionWithoutProofVerification(tx, state);
}


// Test bad Overwinter version numbers in CheckTransactionWithoutProofVerification
TEST(checktransaction_tests, OverwinterVersionNumber) {
    CMutableTransaction mtx = GetValidTransaction();
    mtx.vjoinsplit.resize(0);
    mtx.fOverwintered = true;
    mtx.nVersion = 3;
    mtx.nVersionGroupId = OVERWINTER_VERSION_GROUP_ID;
    mtx.nExpiryHeight = 0;

    {
        CTransaction tx(mtx);

        // Evil
        *const_cast<int32_t*>(&tx.nVersion) = CTransaction::OVERWINTER_MIN_CURRENT_VERSION - 1;

        MockCValidationState state;
        EXPECT_CALL(state, DoS(100, false, REJECT_INVALID, "bad-tx-overwinter-version-too-low", false)).Times(1);
        CheckTransactionWithoutProofVerification(tx, state);
    }

    {
        CTransaction tx(mtx);

        // Evil
        *const_cast<int32_t*>(&tx.nVersion) = CTransaction::OVERWINTER_MAX_CURRENT_VERSION + 1;

        MockCValidationState state;
        EXPECT_CALL(state, DoS(100, false, REJECT_INVALID, "bad-tx-overwinter-version-too-high", false)).Times(1);
        CheckTransactionWithoutProofVerification(tx, state);
    }
}


// Test bad Overwinter version group id
TEST(checktransaction_tests, OverwinterBadVersionGroupId) {
    CMutableTransaction mtx = GetValidTransaction();
    mtx.vjoinsplit.resize(0);
    mtx.fOverwintered = true;
    mtx.nVersion = 3;
    mtx.nVersionGroupId = OVERWINTER_VERSION_GROUP_ID;
    mtx.nExpiryHeight = 0;

    {
        CTransaction tx(mtx);

        // Evil
        *const_cast<uint32_t*>(&tx.nVersionGroupId) = 0x12345678;

        MockCValidationState state;
        EXPECT_CALL(state, DoS(100, false, REJECT_INVALID, "bad-tx-version-group-id", false)).Times(1);
        CheckTransactionWithoutProofVerification(tx, state);
    }
}

// This tests an Overwinter transaction checked against Sprout
TEST(checktransaction_tests, OverwinterNotActive) {
    SelectParams(CBaseChainParams::TESTNET);

    CMutableTransaction mtx = GetValidTransaction();
    mtx.fOverwintered = true;
    mtx.nVersion = 3;
    mtx.nVersionGroupId = OVERWINTER_VERSION_GROUP_ID;
    mtx.nExpiryHeight = 0;

    CTransaction tx(mtx);
    MockCValidationState state;
    EXPECT_CALL(state, DoS(100, false, REJECT_INVALID, "tx-overwinter-not-active", false)).Times(1);
    ContextualCheckTransaction(tx, state, 1, 100);
}

// This tests a transaction without the fOverwintered flag set, against the Overwinter consensus rule set.
TEST(checktransaction_tests, OverwinterFlagNotSet) {
    SelectParams(CBaseChainParams::REGTEST);
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_OVERWINTER, Consensus::NetworkUpgrade::ALWAYS_ACTIVE);

    CMutableTransaction mtx = GetValidTransaction();
    mtx.fOverwintered = false;
    mtx.nVersion = 3;
    mtx.nVersionGroupId = OVERWINTER_VERSION_GROUP_ID;
    mtx.nExpiryHeight = 0;

    CTransaction tx(mtx);
    MockCValidationState state;
    EXPECT_CALL(state, DoS(100, false, REJECT_INVALID, "tx-overwinter-flag-not-set", false)).Times(1);
    ContextualCheckTransaction(tx, state, 1, 100);

    // Revert to default
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_OVERWINTER, Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT);
}


// Contextual test an Overwintered transaction with a bad version number 
TEST(checktransaction_tests, OverwinteredVersionTooLow) {
    SelectParams(CBaseChainParams::REGTEST);
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_OVERWINTER, Consensus::NetworkUpgrade::ALWAYS_ACTIVE);

    CMutableTransaction mtx = GetValidTransaction();
    mtx.fOverwintered = true;
    mtx.nVersion = 3;
    mtx.nVersionGroupId = OVERWINTER_VERSION_GROUP_ID;
    mtx.nExpiryHeight = 0;

    {
        CTransaction tx(mtx);

        // Evil
        *const_cast<int32_t*>(&tx.nVersion) = CTransaction::OVERWINTER_MIN_CURRENT_VERSION - 1;

        MockCValidationState state;
        EXPECT_CALL(state, DoS(100, false, REJECT_INVALID, "tx-overwinter-version-too-low", false)).Times(1);
        ContextualCheckTransaction(tx, state, 1, 100);
    }

    {
        CTransaction tx(mtx);

        // Evil
        *const_cast<int32_t*>(&tx.nVersion) = CTransaction::OVERWINTER_MAX_CURRENT_VERSION + 1;

        MockCValidationState state;
        EXPECT_CALL(state, DoS(100, false, REJECT_INVALID, "tx-overwinter-version-too-high", false)).Times(1);
        ContextualCheckTransaction(tx, state, 1, 100);
    }

    // Revert to default
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_OVERWINTER, Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT);
}


// Overwinter (NU0) does not allow soft fork to version 4 Overwintered tx.
TEST(checktransaction_tests, OverwinterInvalidSoftForkVersion) {
    CMutableTransaction mtx = GetValidTransaction();
    mtx.fOverwintered = true;
    mtx.nVersion = 4; // This is not allowed
    mtx.nVersionGroupId = OVERWINTER_VERSION_GROUP_ID;
    mtx.nExpiryHeight = 0;

    CDataStream ss(SER_DISK, PROTOCOL_VERSION);
    try {
        ss << mtx;
        FAIL() << "Expected std::ios_base::failure";
    }
    catch(std::ios_base::failure & err) {
        EXPECT_EQ(err.what(), std::string("Unknown transaction format: iostream error"));
    }
    catch(...) {
        FAIL() << "Expected std::ios_base::failure, got some other exception";
    }
}


// Test CreateNewContextualCMutableTransaction sets default values based on height
TEST(checktransaction_tests, OverwinteredContextualCreateTx) {
    SelectParams(CBaseChainParams::REGTEST);
    const Consensus::Params& consensusParams = Params().GetConsensus();
    int activationHeight = 5;
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_OVERWINTER, activationHeight);

    {
        CMutableTransaction mtx = CreateNewContextualCMutableTransaction(
            consensusParams, activationHeight - 1);

        EXPECT_EQ(mtx.nVersion, 1);
        EXPECT_EQ(mtx.fOverwintered, false);
        EXPECT_EQ(mtx.nVersionGroupId, 0);
        EXPECT_EQ(mtx.nExpiryHeight, 0);
    }

    // Overwinter activates
    {
        CMutableTransaction mtx = CreateNewContextualCMutableTransaction(
            consensusParams, activationHeight );

        EXPECT_EQ(mtx.nVersion, 3);
        EXPECT_EQ(mtx.fOverwintered, true);
        EXPECT_EQ(mtx.nVersionGroupId, OVERWINTER_VERSION_GROUP_ID);
        EXPECT_EQ(mtx.nExpiryHeight, 0);
    }

    // Revert to default
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_OVERWINTER, Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT);
}
