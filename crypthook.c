/* 
 * CryptHook
 * Secure TCP/UDP wrapper
 * www.chokepoint.net
 * Tested with both blowfish and AES algorithms
 * Example:
 * $ LD_PRELOAD=crypthook.so CH_KEY=omghax ncat -l -p 5000
 * $ LD_PRELOAD=crypthook.so CH_KEY=omghax ncat localhost 5000
 * Packet Format:
 * [algo][iv][hmac][payload]
 */
#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/socket.h>
#include <dlfcn.h>
#include <rhash/rhash.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <string.h>

#include <sys/stat.h>

/* Use these to link to actual functions */
static ssize_t (*old_recv)(int sockfd, void *buf, size_t len, int flags);
static ssize_t (*old_send)(int sockfd, void *buf, size_t len, int flags);
static ssize_t (*old_recvfrom)(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen);
static ssize_t (*old_sendto)(int sockfd, void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen);

#define KEY_VAR "CH_KEY"
#define PASSPHRASE "Hello NSA"
#define MAX_LEN 65535
					
#define KEY_SIZE 32  	
	
#define PACKET_HEADER 0x17		// Added to front of each packet

#define IV_RAND 8 // use 8 bytes of random data to derive the IV on the receiving end
#define IV_SALT "changeme" // Used for deriving the IV
#define IV_SIZE 12

// 1 byte packet identifier
// 8 bytes random data
// 16 bytes authentication
#define HEADER_SIZE 25 

// Used in PBKDF2 key generation
#define ITERATIONS 1000					
			
/* Check environment variables
 * CH_KEY should be the base pass phrase	
 * if key isn't given, revert back to PASSPHRASE.
 * Remember to change the salt
 */
void gen_key(char *phrase, int len) {
	char *key_var = getenv(KEY_VAR);
	const unsigned char salt[]="changeme"; // salt should be changed. both sides need the same salt.
	
	if (key_var) {
		PKCS5_PBKDF2_HMAC_SHA1(key_var,strlen(key_var),salt,strlen((char*)salt),ITERATIONS,KEY_SIZE,(unsigned char *)phrase);
	} else {
		PKCS5_PBKDF2_HMAC_SHA1(PASSPHRASE,strlen(PASSPHRASE),salt,strlen((char*)salt),ITERATIONS,KEY_SIZE,(unsigned char *)phrase);
	}
}

void gen_iv(int new, char *iv, int len, unsigned char *random_data) {
	// Generate random bytes if we're sending
	if (new == 1) {
		RAND_bytes(random_data,IV_RAND);
	} 
	
	PKCS5_PBKDF2_HMAC_SHA1(random_data,IV_RAND,IV_SALT,strlen(IV_SALT),ITERATIONS,IV_SIZE,(unsigned char *)iv);
}

int encrypt_data(char *in, int len, char *out) {
	unsigned char outbuf[MAX_LEN];
	unsigned char temp[MAX_LEN];
	unsigned char iv[IV_SIZE];
	unsigned char key[KEY_SIZE];
	unsigned char random_data[IV_RAND];
	unsigned char tag[16];
	
	unsigned char *step;
	int tmplen=0, outlen=0;

	// copy plain text message into temp
	memset(temp,0x00,MAX_LEN);
	memcpy(temp,in,len);
	
	gen_key(key,KEY_SIZE); // Determine key based on environment 
	gen_iv(1,iv,IV_SIZE,random_data);
	
	EVP_CIPHER_CTX *ctx;
	ctx = EVP_CIPHER_CTX_new();
	EVP_CIPHER_CTX_init (ctx);
	EVP_EncryptInit_ex (ctx, EVP_aes_256_gcm() , NULL, (const unsigned char *)key, (const unsigned char *)iv);

	if (!EVP_EncryptUpdate (ctx, outbuf, &outlen, (const unsigned char *)temp, len)) {
		fprintf(stderr, "[!] Error in EVP_EncryptUpdate()\n");
		EVP_CIPHER_CTX_cleanup (ctx);
		return 0;
	}

	if (!EVP_EncryptFinal_ex (ctx, outbuf + outlen, &tmplen)) {
		fprintf(stderr, "[!] Error in EVP_EncryptFinal_ex()\n");
		EVP_CIPHER_CTX_cleanup (ctx);
		return 0;
	}
	
	EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag);
	
	out[0]=PACKET_HEADER;
	step=&out[1];	
	memcpy(step,random_data,IV_RAND);
	step+=IV_RAND;
	memcpy(step,tag,sizeof(tag));
	step+=sizeof(tag);
	memcpy(step,outbuf,outlen+tmplen);
	
	EVP_CIPHER_CTX_cleanup (ctx);
	return outlen+tmplen+HEADER_SIZE;
}

int decrypt_data(char *in, int len, char *out) {
	unsigned char outbuf[MAX_LEN];
	unsigned char iv[IV_SIZE];
	unsigned char key[KEY_SIZE];
	unsigned char random_data[IV_RAND];
	unsigned char tag[16];
	char *step;
	
	int tmplen=0, outlen=0;
	
	memset(outbuf,0x00,MAX_LEN);
	
	// header information
	step=in+1;
	memcpy(random_data,step,IV_RAND);
	step+=IV_RAND;
	memcpy(tag,step,16);
	step+=16;

	gen_key(key,KEY_SIZE); // Determine key based on environment 
	gen_iv(0,iv,IV_SIZE,random_data);
	
	EVP_CIPHER_CTX *ctx;
	ctx = EVP_CIPHER_CTX_new();
	EVP_CIPHER_CTX_init (ctx);
	EVP_DecryptInit_ex (ctx, EVP_aes_256_gcm() , NULL, (const unsigned char *)key, (const unsigned char *)iv);
	
	EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, IV_SIZE, NULL);
	
	if (!EVP_DecryptUpdate (ctx, outbuf, &outlen, (const unsigned char *)step, len)) {
		fprintf(stderr, "[!] Error in EVP_DecryptUpdate()\n");
		EVP_CIPHER_CTX_cleanup (ctx);
		return 0;
	}

	EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, sizeof(tag), tag);

	if (!EVP_DecryptFinal_ex (ctx, outbuf + outlen, &tmplen)) {
		fprintf(stderr, "[!] Error in EVP_DecryptFinal_ex(). Possible foul play involved.\n");
		EVP_CIPHER_CTX_cleanup (ctx);
		return 0;
	}
	
	EVP_CIPHER_CTX_cleanup (ctx);
	
	memcpy(out,outbuf,outlen+tmplen);
	
	return len;
}

/* Hook recv and decrypt the data before returning to the program */
ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
	char outbuf[MAX_LEN];
	char temp[MAX_LEN];
	char *step;
	
	int outlen, ret;

	memset(outbuf,0x00,MAX_LEN);
	memset(temp,0x00,MAX_LEN);
	
	if (!old_recv)
		old_recv = dlsym(RTLD_NEXT,"recv");
		
	if (sockfd == 0) // Y U CALL ME W/ SOCKFD SET TO ZERO!?!?
		return old_recv(sockfd, buf, len, flags);
	
	ret = old_recv(sockfd, (void *)temp, MAX_LEN, flags);
	
	if (ret < 1) { // Nothing to decrypt 
		return ret;
	}

	if (temp[0] != PACKET_HEADER) {
		fprintf(stderr,"[!] Client not using CryptHook\n");
		return 0;
	}
	step=&temp[0];

	outlen = decrypt_data(step,ret - HEADER_SIZE,&outbuf[0]);

	memcpy((void*)buf,(void*)outbuf,(size_t)outlen);
	
	return outlen;
}

/* Hook recvfrom and decrypt the data before returning to the program */
ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen) {	
	char outbuf[MAX_LEN];
	char temp[MAX_LEN];
	char *step;
	
	int outlen, ret;

	memset(outbuf,0x00,MAX_LEN);
	memset(temp,0x00,MAX_LEN);
	
	if (!old_recvfrom)
		old_recvfrom = dlsym(RTLD_NEXT,"recvfrom");
		
	if (sockfd == 0) // Y U CALL ME W/ SOCKFD SET TO ZERO!?!?
		return old_recvfrom(sockfd, buf, len, flags, src_addr, addrlen);
	
	ret = old_recvfrom(sockfd, (void *)temp, MAX_LEN, flags, src_addr, addrlen);
	
	if (ret < 1) { // Nothing to decrypt 
		return ret;
	}

	if (temp[0] != PACKET_HEADER) {
		fprintf(stderr,"[!] Client not using same crypto algorithm\n");
		return 0;
	}
	step=&temp[0];

	outlen = decrypt_data(step,ret-HEADER_SIZE,&outbuf[0]);

	memcpy((void*)buf,(void*)outbuf,(size_t)outlen);
	
	return outlen;
}

/* Hook send and encrypt data first */
ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
	char outbuf[MAX_LEN];
	int outlen;
	
	memset(outbuf,0x00,MAX_LEN);
	
	if (!old_send)
		old_send = dlsym(RTLD_NEXT,"send");
		
	outlen = encrypt_data((char *)buf, len, &outbuf[0]);
	if (outlen == 0)
		return 0;
		
	// Send the encrypted data
	old_send(sockfd, (void *)outbuf, outlen, flags);

	return len; 
}

/* Hook send and encrypt data first */
ssize_t sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen) {
	char outbuf[MAX_LEN];
	int outlen;
	
	memset(outbuf,0x00,MAX_LEN);
	
	if (!old_sendto)
		old_sendto = dlsym(RTLD_NEXT,"sendto");
		
	outlen = encrypt_data((char *)buf, len, &outbuf[0]);
	if (outlen == 0)
		return 0;
		
	// Send the encrypted data
	old_sendto(sockfd, (void *)outbuf, outlen, flags, dest_addr, addrlen);

	return len; 
}
