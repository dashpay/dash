
# How to Create a New Alt Coin

Since there are so many garbage coins out there, I decide to create
this course to teach you how to create a new alt coin. It's so simple,
that I can usually do within 2 hours.

You need to have some basic knowledge of C++ programming. No need
to be an expert, but you need to understand the basic compiling errors
and how to fix them.

Follow the following steps to create a new alt coin:

## Preparation Step:

Download the Litecoin full source code, you can find it in github, just
do a google, you'll find it.

If you use windows, follow the steps in this thread:
https://bitcointalk.org/index.php?topic=149479.0

set up environment and libs, and compile the Litecoin. If you are successful 
in compiling it (and run it), then your environment is good.

## Design your coin:

Now that you have env set up, before doing coding, you need to do some design of 
your coin, this is simple math calculations, but you need them for parameters
in the coding.

### Basically you need to determine what you want:
- name of the coin. Let's call it AbcCoin in our example. Also determine the symbol, we call it "ABC"
   in our example.
- block time: this is the average target time between blocks. Usually you set it between 15 sec and 2 mins.
   You can also do 10 mins if you want (like bitcoin), but it's too long imo.
- difficulty retarget time: this is important, since otherwise it could cause serious instamine problem.
   Usually it's between 1 hr to 1 day.
   (diff retarget time)/(block time) gives you how many blocks a diff retarget will happen. This is
   an important parameter to consider.
- what's the initial coin per block. People set it to 2 to 100, usually. You can set any you want.
   Also you can do coins per block based on the block number, or even randomly (like JKC/LKY etc).
- How long you want coins per block be halved. Usually it's 6 month to 3 years. But again you set whatever 
   you like.
- Ports, you need two: connection and RPC ports. Choose the ones that are not used in common apps.
   You can google for a particular port usage.

There are some other things you may want to adjust, such as initial diffculty etc. But usually I don't want to bother with these.

Now with these parameters defined, one important thing is that you want to calculate how many blocks/coins
generated in a month, a year etc, and total coins ever will be generated. This gives you a good idea how overall
your coin will work, and you may want to re-adjust some parameters above.

## Now the code change part.

Before you begin, copy the whole directory of Litecoin to Abccoin. Then modify the code in Abccoin.

Follow the below steps for code changes:

1. In Abccoin/src dir, do a search of "litecoin", and change most of them to "abccoin", note you may want to do a case-sensitive replace. You don't have to replace all, but most should be replaced. You can reference to smallchange code first commit
https://github.com/bfroemel/smallchange/commit/947a0fafd8d033f6f0960c4ff0748f76a3d58326
for the changes needed.
Note: smallchange 1st commit does not include many of the changes I will outline below, but it is a good reference for what need to be changed.

2. In Abccoin/src dir, do a search on "LTC", and change them to "ABC".

3. Change the ports: use the ones you defined in coin design, and change in the following files:
 - connection port: protocol.h and init.cpp
 - rpc port: bitcoinrpc.cpp and init.cpp

4.  Change parameters, all in main.cpp:
   - block value (in GetBlockValue())
   - block time (right after GetBlockValue())
   - diff retarget time (right after GetBlockValue())
   - adjust the diff retarget scale to avoid instamine (in GetNextWorkRequired())

for the last item, refer to Luckycoin code, you will see how this is done.
For random coin values in block, refer to GetBlockValue() function in JKC and Luckycoin code.

5. According to your coin design, adjust the value in main.h:
   - max coin count
   - dPriority

6. Change transaction confirmation count (if you want say 3 confirmation transaction etc) in transactionrecord.h
also change COINBASE_MATURITY which affects the maturity time for mined blocks, in main.h/cpp.

7.  Create genesis block. Some people get stuck there, it's really easy:
    - find LoadBlockIndex() function, inside, change:
    - paraphrase (pszTimestamp) to any recent news phase.
    - get the latest unix time (do a google), and put in block.nTime.
    - set any nNonce (doesn't really matter)

you can change the time/nonce for testnet too, if you want to use it.

After you are done, save it. Now the genesis block will not match the hash check and merkle root check, it doesn't matter.

The first time you run the compiled code (daemon or qt), it will say "assertion failed". Just exit the program, go to
config dir (under AppData/Roaming), open the debug.log, get the hash after "block.GetHash() = ", copy and paste it to the beginnig of main.cpp, hashGenesisBlock. Also get the merkle root in the same log file, paste it to the ... position in the following code, in LoadBlockIndex()
Quote
       assert(block.hashMerkleRoot == uint256("0x..."));

recompile the code, and genesis block created!

BTW, don't forget to change "txNew.vout[0].nValue = " to the coin per block you defined, it doesn't matter to leave as 50, just be consistent with your coin per block (do this before adjust the hash and m-root, otherwise they will be changed again).

Also you need to change the alert/checkpoint key, this depends on the coin type and version, you can find it in main.cpp, main.h, alert.cpp and checkpoint.cpp.

8. Set the correct address start letter in base58.h. You may want to do some trial and error to find the letter you want. I never can calculate precisely the letter location.

change corresponding "starts with " in sendcoinsentry.cpp
change example in signverifymessagedialog.cpp

9. Checkpoint: you want to disable the checkpoint check initially, otherwise you may get stuck.
You have multiple ways to disable it, my way is:
- open checkpoints.cpp
- there are 3 functions, comment out the normal return, and make them return either true, 0, or null, like this:
Quote
```cpp
    bool CheckBlock(int nHeight, const uint256& hash)
    {
        if (fTestNet) return true; // Testnet has no checkpoints
        MapCheckpoints::const_iterator i = mapCheckpoints.find(nHeight);
        if (i == mapCheckpoints.end()) return true;
        // return hash == i->second;
        return true;
    }
    
    int GetTotalBlocksEstimate()
    {
        if (fTestNet) return 0;
    
        // return mapCheckpoints.rbegin()->first;
        return 0;
    }
    
    CBlockIndex* GetLastCheckpoint(const std::map<uint256, CBlockIndex*>& mapBlockIndex)
    {
        if (fTestNet) return NULL;
    
        BOOST_REVERSE_FOREACH(const MapCheckpoints::value_type& i, mapCheckpoints)
        {
            const uint256& hash = i.second;
            std::map<uint256, CBlockIndex*>::const_iterator t = mapBlockIndex.find(hash);
            if (t != mapBlockIndex.end())
                // return t->second;
            return NULL;
        }
        return NULL;
    }
```
Now this is disabled. Once everything works, you can premine 50 blocks, and extract some hashes and put them in the checkpoints, and re-enable these functions.

That's about it. You can do compilation all the way along, no need to do in the end, you may get a lot compilation errors.

Oh, icons:

10. Find a nice image for your coin, then make 256x256 icons/images. You have 5 images to replace in src/qt/res/icons, and 1 to replace (splash) in src/qt/res/images.

11. Oh also edit those files in qt/forms. These are files for help etc, better make them look nice, display your coin names than litecoin ones.

12. Now for compilations:
- qt: modify the .pro file under abccoin, and follow the make process in 
https://bitcointalk.org/index.php?topic=149479.0

- daemon: update one of the makefile for your system, and in my case I use mingw32 shell to make.


That's it, voila, you have your own alt coins!!