// SPDX-License-Identifier: GPL-2.0-only
/*
 * This contains encryption functions for per-file encryption.
 *
 * Copyright (C) 2015, Google, Inc.
 * Copyright (C) 2015, Motorola Mobility
 *
 * Written by Michael Halcrow, 2014.
 *
 * Filename encryption additions
 *	Uday Savagaonkar, 2014
 * Encryption policy handling additions
 *	Ildar Muslukhov, 2014
 * Add fscrypt_pullback_bio_page()
 *	Jaegeuk Kim, 2015.
 *
 * This has not yet undergone a rigorous security audit.
 *
 * The usage of AES-XTS should conform to recommendations in NIST
 * Special Publication 800-38E and IEEE P1619/D16.
 */

#include <linux/pagemap.h>
#include <linux/mempool.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/ratelimit.h>
#include <crypto/skcipher.h>
#include "fscrypt_private.h"

#define DEDUP_TABLE_SIZE 1024*1024
extern struct Fingeritem *hashArray[DEDUP_TABLE_SIZE];
extern struct Fingercryptitem *hashcryptArray[DEDUP_TABLE_SIZE];
extern struct Blkrefitem *blkArray[DEDUP_TABLE_SIZE];

struct Cryptitem {
	char fingerprint_crypt[16];
	unsigned long ino;
};
struct Fingeritem {
	char fingerprint[16];
	size_t blk_addr;
};
struct Fingercryptitem {
	// 用于解密时获得数据块原本的lblk_num
	char fingerprint_crypt[16];
	u64 lblk_num;
};
struct Blkrefitem {
	size_t blk_addr;
	size_t ref;
};

extern struct file *finger_table;
extern struct file *finger_crypt_table;
extern struct file *block_table;


struct Cryptitem *cryptArray[DEDUP_TABLE_SIZE];

size_t hashCrypt(char *finger){
	size_t hash = 0;
	for (size_t i = 0; i < 16; i++)
		hash += (size_t)finger[i];
	hash = hash % DEDUP_TABLE_SIZE;
	return hash;
}

struct Cryptitem *crypt_search(char *finger)
{
	//get the hash
	size_t hashIndex = hashCrypt(finger);
	int loop = 0;
	//move in array until an empty
	while (cryptArray[hashIndex] != NULL) {
		int ret =
			strncmp(cryptArray[hashIndex]->fingerprint_crypt, finger, 16);
		if (!ret)
			return cryptArray[hashIndex];

		//go to next cell
		++hashIndex;
		loop++;
		if (loop > DEDUP_TABLE_SIZE)
			return NULL;

		//wrap around the table
		hashIndex %= DEDUP_TABLE_SIZE;
	}
	return NULL;
}

int crypttable_insert(char *finger, unsigned long ino) {
	int loop;
	int hashIndex;
	struct Cryptitem *crypt_item = (struct Cryptitem *)kmalloc(sizeof(struct Cryptitem), GFP_KERNEL);
	strncpy(crypt_item->fingerprint_crypt, finger, 16);
	crypt_item->ino = ino;

	hashIndex = hashCrypt(finger);
	loop = 0;
	while (cryptArray[hashIndex] != NULL && cryptArray[hashIndex]->ino > 0) {
		//go to next cell
		++hashIndex;

		//wrap around the table
		hashIndex %= DEDUP_TABLE_SIZE;
		loop++;
		if (loop > DEDUP_TABLE_SIZE)
			return 1;
	}
	kfree(cryptArray[hashIndex]);
	cryptArray[hashIndex] = crypt_item;
	return 0;
}

void read_finger_blk_table_from_file(void);
void write_finger_blk_table_to_file(void);
size_t hashFinger(char *finger);
size_t hashBlk(size_t blk_addr);
struct Fingercryptitem *finger_crypt_search(char *finger);
struct Fingeritem *finger_search(char *finger);
struct Blkrefitem *block_search(size_t blk_addr);
void __attribute__((optimize("O0"))) hash_page_data(struct page* page, char* digest);
struct inode *f2fs_iget(struct super_block *sb, unsigned long ino);


static unsigned int num_prealloc_crypto_pages = 32;

module_param(num_prealloc_crypto_pages, uint, 0444);
MODULE_PARM_DESC(num_prealloc_crypto_pages,
		"Number of crypto pages to preallocate");

static mempool_t *fscrypt_bounce_page_pool = NULL;

static struct workqueue_struct *fscrypt_read_workqueue;
static DEFINE_MUTEX(fscrypt_init_mutex);

struct kmem_cache *fscrypt_info_cachep;

void fscrypt_enqueue_decrypt_work(struct work_struct *work)
{
	queue_work(fscrypt_read_workqueue, work);
}
EXPORT_SYMBOL(fscrypt_enqueue_decrypt_work);

struct page *fscrypt_alloc_bounce_page(gfp_t gfp_flags)
{
	return mempool_alloc(fscrypt_bounce_page_pool, gfp_flags);
}

/**
 * fscrypt_free_bounce_page() - free a ciphertext bounce page
 * @bounce_page: the bounce page to free, or NULL
 *
 * Free a bounce page that was allocated by fscrypt_encrypt_pagecache_blocks(),
 * or by fscrypt_alloc_bounce_page() directly.
 */
void fscrypt_free_bounce_page(struct page *bounce_page)
{
	if (!bounce_page)
		return;
	set_page_private(bounce_page, (unsigned long)NULL);
	ClearPagePrivate(bounce_page);
	mempool_free(bounce_page, fscrypt_bounce_page_pool);
}
EXPORT_SYMBOL(fscrypt_free_bounce_page);

/*
 * Generate the IV for the given logical block number within the given file.
 * For filenames encryption, lblk_num == 0.
 *
 * Keep this in sync with fscrypt_limit_io_blocks().  fscrypt_limit_io_blocks()
 * needs to know about any IV generation methods where the low bits of IV don't
 * simply contain the lblk_num (e.g., IV_INO_LBLK_32).
 */
void fscrypt_generate_iv(union fscrypt_iv *iv, u64 lblk_num,
			 const struct fscrypt_info *ci)
{
	u8 flags = fscrypt_policy_flags(&ci->ci_policy);

	memset(iv, 0, ci->ci_mode->ivsize);

	if (flags & FSCRYPT_POLICY_FLAG_IV_INO_LBLK_64) {
		WARN_ON_ONCE(lblk_num > U32_MAX);
		WARN_ON_ONCE(ci->ci_inode->i_ino > U32_MAX);
		lblk_num |= (u64)ci->ci_inode->i_ino << 32;
	} else if (flags & FSCRYPT_POLICY_FLAG_IV_INO_LBLK_32) {
		WARN_ON_ONCE(lblk_num > U32_MAX);
		lblk_num = (u32)(ci->ci_hashed_ino + lblk_num);
	} else if (flags & FSCRYPT_POLICY_FLAG_DIRECT_KEY) {
		memcpy(iv->nonce, ci->ci_nonce, FSCRYPT_FILE_NONCE_SIZE);
	}
	iv->lblk_num = cpu_to_le64(lblk_num);
}

/* Encrypt or decrypt a single filesystem block of file contents */
int fscrypt_crypt_block(const struct inode *inode, fscrypt_direction_t rw,
			u64 lblk_num, struct page *src_page,
			struct page *dest_page, unsigned int len,
			unsigned int offs, gfp_t gfp_flags)
{
	union fscrypt_iv iv;
	struct skcipher_request *req = NULL;
	DECLARE_CRYPTO_WAIT(wait);
	struct scatterlist dst, src;
	struct fscrypt_info *ci = inode->i_crypt_info;
	struct crypto_skcipher *tfm = ci->ci_enc_key.tfm;
	int res = 0;

	char digest[16];
	struct Fingercryptitem *finger_crypt_item;
	struct Cryptitem *crypt_item;
	// 解密时读取hashtable，获取lblk_num
	if (rw == FS_DECRYPT) {
		read_finger_blk_table_from_file();
		hash_page_data(src_page, digest);
		finger_crypt_item = finger_crypt_search(digest);
		if (finger_crypt_item) {
			lblk_num = finger_crypt_item->lblk_num;
		}
	}

	if (WARN_ON_ONCE(len <= 0))
		return -EINVAL;
	if (WARN_ON_ONCE(len % FSCRYPT_CONTENTS_ALIGNMENT != 0))
		return -EINVAL;

	fscrypt_generate_iv(&iv, lblk_num, ci);

	req = skcipher_request_alloc(tfm, gfp_flags);
	if (!req)
		return -ENOMEM;

	skcipher_request_set_callback(
		req, CRYPTO_TFM_REQ_MAY_BACKLOG | CRYPTO_TFM_REQ_MAY_SLEEP,
		crypto_req_done, &wait);

	sg_init_table(&dst, 1);
	sg_set_page(&dst, dest_page, len, offs);
	sg_init_table(&src, 1);
	sg_set_page(&src, src_page, len, offs);
	skcipher_request_set_crypt(req, &src, &dst, len, &iv);
	if (rw == FS_DECRYPT)
		res = crypto_wait_req(crypto_skcipher_decrypt(req), &wait);
	else
		res = crypto_wait_req(crypto_skcipher_encrypt(req), &wait);
	skcipher_request_free(req);
	if (res) {
		fscrypt_err(inode, "%scryption failed for block %llu: %d",
			    (rw == FS_DECRYPT ? "De" : "En"), lblk_num, res);
		return res;
	}
	// 加密完成之后， destpage存储着密文，计算密文的finger，存储ino
	if (rw == FS_ENCRYPT) {
		loff_t pos_ci;
		int hashIndex = 0;
		struct file *ci_table = filp_open("/citable", O_RDWR | O_CREAT, 0);
		for (hashIndex = 0; hashIndex < DEDUP_TABLE_SIZE; hashIndex++) {
			if (cryptArray[hashIndex] == NULL) {
				cryptArray[hashIndex] = (struct Cryptitem *)kmalloc(sizeof(struct Cryptitem), GFP_KERNEL);
				cryptArray[hashIndex]->ino = 0;
			}
		}
		for (hashIndex = 0, pos_ci = 0; hashIndex < DEDUP_TABLE_SIZE; hashIndex++) {
			kernel_read(ci_table, cryptArray[hashIndex], sizeof(struct Cryptitem), &pos_ci);
		}
		hash_page_data(dest_page, digest);
		if (!crypt_search(digest)) {
			crypttable_insert(digest, inode->i_ino);
		}
		for (hashIndex = 0, pos_ci = 0; hashIndex < DEDUP_TABLE_SIZE; hashIndex++) {
			kernel_write(ci_table, cryptArray[hashIndex], sizeof(struct Cryptitem), &pos_ci);
		}
		filp_close(ci_table, NULL);
	}
	return 0;
}

/**
 * fscrypt_encrypt_pagecache_blocks() - Encrypt filesystem blocks from a
 *					pagecache page
 * @page:      The locked pagecache page containing the block(s) to encrypt
 * @len:       Total size of the block(s) to encrypt.  Must be a nonzero
 *		multiple of the filesystem's block size.
 * @offs:      Byte offset within @page of the first block to encrypt.  Must be
 *		a multiple of the filesystem's block size.
 * @gfp_flags: Memory allocation flags.  See details below.
 *
 * A new bounce page is allocated, and the specified block(s) are encrypted into
 * it.  In the bounce page, the ciphertext block(s) will be located at the same
 * offsets at which the plaintext block(s) were located in the source page; any
 * other parts of the bounce page will be left uninitialized.  However, normally
 * blocksize == PAGE_SIZE and the whole page is encrypted at once.
 *
 * This is for use by the filesystem's ->writepages() method.
 *
 * The bounce page allocation is mempool-backed, so it will always succeed when
 * @gfp_flags includes __GFP_DIRECT_RECLAIM, e.g. when it's GFP_NOFS.  However,
 * only the first page of each bio can be allocated this way.  To prevent
 * deadlocks, for any additional pages a mask like GFP_NOWAIT must be used.
 *
 * Return: the new encrypted bounce page on success; an ERR_PTR() on failure
 */
struct page *fscrypt_encrypt_pagecache_blocks(struct page *page,
					      unsigned int len,
					      unsigned int offs,
					      gfp_t gfp_flags)

{
	const struct inode *inode = page->mapping->host;
	const unsigned int blockbits = inode->i_blkbits;
	const unsigned int blocksize = 1 << blockbits;
	struct page *ciphertext_page;
	u64 lblk_num = ((u64)page->index << (PAGE_SHIFT - blockbits)) +
		       (offs >> blockbits);
	unsigned int i;
	int err;

	if (WARN_ON_ONCE(!PageLocked(page)))
		return ERR_PTR(-EINVAL);

	if (WARN_ON_ONCE(len <= 0 || !IS_ALIGNED(len | offs, blocksize)))
		return ERR_PTR(-EINVAL);

	ciphertext_page = fscrypt_alloc_bounce_page(gfp_flags);
	if (!ciphertext_page)
		return ERR_PTR(-ENOMEM);

	for (i = offs; i < offs + len; i += blocksize, lblk_num++) {
		err = fscrypt_crypt_block(inode, FS_ENCRYPT, lblk_num,
					  page, ciphertext_page,
					  blocksize, i, gfp_flags);
		if (err) {
			fscrypt_free_bounce_page(ciphertext_page);
			return ERR_PTR(err);
		}
	}
	SetPagePrivate(ciphertext_page);
	set_page_private(ciphertext_page, (unsigned long)page);
	return ciphertext_page;
}
EXPORT_SYMBOL(fscrypt_encrypt_pagecache_blocks);

/**
 * fscrypt_encrypt_block_inplace() - Encrypt a filesystem block in-place
 * @inode:     The inode to which this block belongs
 * @page:      The page containing the block to encrypt
 * @len:       Size of block to encrypt.  This must be a multiple of
 *		FSCRYPT_CONTENTS_ALIGNMENT.
 * @offs:      Byte offset within @page at which the block to encrypt begins
 * @lblk_num:  Filesystem logical block number of the block, i.e. the 0-based
 *		number of the block within the file
 * @gfp_flags: Memory allocation flags
 *
 * Encrypt a possibly-compressed filesystem block that is located in an
 * arbitrary page, not necessarily in the original pagecache page.  The @inode
 * and @lblk_num must be specified, as they can't be determined from @page.
 *
 * Return: 0 on success; -errno on failure
 */
int fscrypt_encrypt_block_inplace(const struct inode *inode, struct page *page,
				  unsigned int len, unsigned int offs,
				  u64 lblk_num, gfp_t gfp_flags)
{
	return fscrypt_crypt_block(inode, FS_ENCRYPT, lblk_num, page, page,
				   len, offs, gfp_flags);
}
EXPORT_SYMBOL(fscrypt_encrypt_block_inplace);

/**
 * fscrypt_decrypt_pagecache_blocks() - Decrypt filesystem blocks in a
 *					pagecache page
 * @page:      The locked pagecache page containing the block(s) to decrypt
 * @len:       Total size of the block(s) to decrypt.  Must be a nonzero
 *		multiple of the filesystem's block size.
 * @offs:      Byte offset within @page of the first block to decrypt.  Must be
 *		a multiple of the filesystem's block size.
 *
 * The specified block(s) are decrypted in-place within the pagecache page,
 * which must still be locked and not uptodate.  Normally, blocksize ==
 * PAGE_SIZE and the whole page is decrypted at once.
 *
 * This is for use by the filesystem's ->readahead() method.
 *
 * Return: 0 on success; -errno on failure
 */
int fscrypt_decrypt_pagecache_blocks(struct page *page, unsigned int len,
				     unsigned int offs)
{
	// 这里的inode删掉了const关键字
	struct inode *inode = page->mapping->host;
	const unsigned int blockbits = inode->i_blkbits;
	const unsigned int blocksize = 1 << blockbits;
	u64 lblk_num = ((u64)page->index << (PAGE_SHIFT - blockbits)) +
		       (offs >> blockbits);
	unsigned int i;
	int err;

	char digest_c[16];
	loff_t pos_ci;
	int hashIndex = 0;
	struct Cryptitem *crypt_item;
	struct file *ci_table = filp_open("/citable", O_RDWR | O_CREAT, 0);

	if (WARN_ON_ONCE(!PageLocked(page)))
		return -EINVAL;

	if (WARN_ON_ONCE(len <= 0 || !IS_ALIGNED(len | offs, blocksize)))
		return -EINVAL;

	// 从文件中恢复hashtable，读取加密page对应的ino
	for (hashIndex = 0; hashIndex < DEDUP_TABLE_SIZE; hashIndex++) {
		if (cryptArray[hashIndex] == NULL) {
			cryptArray[hashIndex] = (struct Cryptitem *)kmalloc(sizeof(struct Cryptitem), GFP_KERNEL);
			cryptArray[hashIndex]->ino = 0;
		}
	}
	for (hashIndex = 0, pos_ci = 0; hashIndex < DEDUP_TABLE_SIZE; hashIndex++) {
		kernel_read(ci_table, cryptArray[hashIndex], sizeof(struct Cryptitem), &pos_ci);
	}
	hash_page_data(page, digest_c);
	crypt_item = crypt_search(digest_c);
	if (crypt_item) {
		int err;
		inode = f2fs_iget(inode->i_sb, crypt_item->ino);
		err = fscrypt_require_key(inode);
	}

	for (i = offs; i < offs + len; i += blocksize, lblk_num++) {
		err = fscrypt_crypt_block(inode, FS_DECRYPT, lblk_num, page,
					  page, blocksize, i, GFP_NOFS);
		if (err)
			return err;
	}
	return 0;
}
EXPORT_SYMBOL(fscrypt_decrypt_pagecache_blocks);

/**
 * fscrypt_decrypt_block_inplace() - Decrypt a filesystem block in-place
 * @inode:     The inode to which this block belongs
 * @page:      The page containing the block to decrypt
 * @len:       Size of block to decrypt.  This must be a multiple of
 *		FSCRYPT_CONTENTS_ALIGNMENT.
 * @offs:      Byte offset within @page at which the block to decrypt begins
 * @lblk_num:  Filesystem logical block number of the block, i.e. the 0-based
 *		number of the block within the file
 *
 * Decrypt a possibly-compressed filesystem block that is located in an
 * arbitrary page, not necessarily in the original pagecache page.  The @inode
 * and @lblk_num must be specified, as they can't be determined from @page.
 *
 * Return: 0 on success; -errno on failure
 */
int fscrypt_decrypt_block_inplace(const struct inode *inode, struct page *page,
				  unsigned int len, unsigned int offs,
				  u64 lblk_num)
{
	return fscrypt_crypt_block(inode, FS_DECRYPT, lblk_num, page, page,
				   len, offs, GFP_NOFS);
}
EXPORT_SYMBOL(fscrypt_decrypt_block_inplace);

/**
 * fscrypt_initialize() - allocate major buffers for fs encryption.
 * @cop_flags:  fscrypt operations flags
 *
 * We only call this when we start accessing encrypted files, since it
 * results in memory getting allocated that wouldn't otherwise be used.
 *
 * Return: 0 on success; -errno on failure
 */
int fscrypt_initialize(unsigned int cop_flags)
{
	int err = 0;

	/* No need to allocate a bounce page pool if this FS won't use it. */
	if (cop_flags & FS_CFLG_OWN_PAGES)
		return 0;

	mutex_lock(&fscrypt_init_mutex);
	if (fscrypt_bounce_page_pool)
		goto out_unlock;

	err = -ENOMEM;
	fscrypt_bounce_page_pool =
		mempool_create_page_pool(num_prealloc_crypto_pages, 0);
	if (!fscrypt_bounce_page_pool)
		goto out_unlock;

	err = 0;
out_unlock:
	mutex_unlock(&fscrypt_init_mutex);
	return err;
}

void fscrypt_msg(const struct inode *inode, const char *level,
		 const char *fmt, ...)
{
	static DEFINE_RATELIMIT_STATE(rs, DEFAULT_RATELIMIT_INTERVAL,
				      DEFAULT_RATELIMIT_BURST);
	struct va_format vaf;
	va_list args;

	if (!__ratelimit(&rs))
		return;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	if (inode && inode->i_ino)
		printk("%sfscrypt (%s, inode %lu): %pV\n",
		       level, inode->i_sb->s_id, inode->i_ino, &vaf);
	else if (inode)
		printk("%sfscrypt (%s): %pV\n", level, inode->i_sb->s_id, &vaf);
	else
		printk("%sfscrypt: %pV\n", level, &vaf);
	va_end(args);
}

/**
 * fscrypt_init() - Set up for fs encryption.
 *
 * Return: 0 on success; -errno on failure
 */
static int __init fscrypt_init(void)
{
	int err = -ENOMEM;

	/*
	 * Use an unbound workqueue to allow bios to be decrypted in parallel
	 * even when they happen to complete on the same CPU.  This sacrifices
	 * locality, but it's worthwhile since decryption is CPU-intensive.
	 *
	 * Also use a high-priority workqueue to prioritize decryption work,
	 * which blocks reads from completing, over regular application tasks.
	 */
	fscrypt_read_workqueue = alloc_workqueue("fscrypt_read_queue",
						 WQ_UNBOUND | WQ_HIGHPRI,
						 num_online_cpus());
	if (!fscrypt_read_workqueue)
		goto fail;

	fscrypt_info_cachep = KMEM_CACHE(fscrypt_info, SLAB_RECLAIM_ACCOUNT);
	if (!fscrypt_info_cachep)
		goto fail_free_queue;

	err = fscrypt_init_keyring();
	if (err)
		goto fail_free_info;

	return 0;

fail_free_info:
	kmem_cache_destroy(fscrypt_info_cachep);
fail_free_queue:
	destroy_workqueue(fscrypt_read_workqueue);
fail:
	return err;
}
late_initcall(fscrypt_init)
