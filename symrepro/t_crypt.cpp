// CryptoAPI as SendMail.cpp uses it: key container, the "AddressBook"
// system store, finding a recipient certificate by subject, describing it,
// and encrypting a message to it with 3DES.
#include "harness.h"
#include <wincrypt.h>
#include "certdata.h"

TEST(crypt_KeyContainer)
{
	const wchar_t *name = L"symrepro_container";
	HCRYPTPROV p = 0;
	::CryptAcquireContextW(&p, name, NULL, PROV_RSA_FULL, CRYPT_DELETEKEYSET);	// clean slate
	p = 0;
	BOOL r = ::CryptAcquireContextW(&p, name, NULL, PROV_RSA_FULL, 0);
	out("CryptAcquireContextW", "missing", "%s", r ? "ok" : Fmt("fail err=%08lx", ::GetLastError()).c_str());
	if (r)
		::CryptReleaseContext(p, 0);
	r = ::CryptAcquireContextW(&p, name, NULL, PROV_RSA_FULL, CRYPT_NEWKEYSET);
	out("CryptAcquireContextW", "newkeyset", "%s", r ? "ok" : Fmt("fail err=%08lx", ::GetLastError()).c_str());
	if (r)
		out("CryptReleaseContext", "newkeyset", "%s", B(::CryptReleaseContext(p, 0)).c_str());
	r = ::CryptAcquireContextW(&p, name, NULL, PROV_RSA_FULL, CRYPT_NEWKEYSET);
	out("CryptAcquireContextW", "newkeyset.again", "%s", r ? "ok" : Fmt("fail err=%08lx", ::GetLastError()).c_str());
	if (r)
		::CryptReleaseContext(p, 0);
	r = ::CryptAcquireContextW(&p, name, NULL, PROV_RSA_FULL, 0);
	out("CryptAcquireContextW", "existing", "%s", r ? "ok" : Fmt("fail err=%08lx", ::GetLastError()).c_str());
	if (r) {
		DWORD len = 0;
		BOOL gp = ::CryptGetProvParam(p, PP_NAME, NULL, &len, 0);
		std::string pn(len, '\0');
		gp = ::CryptGetProvParam(p, PP_NAME, (BYTE*)&pn[0], &len, 0);
		out("CryptAcquireContextW", "~provider", "%s", gp ? QA(pn.c_str()).c_str() : "?");
		::CryptReleaseContext(p, 0);
	}
	HCRYPTPROV pa = 0;
	r = ::CryptAcquireContextA(&pa, "symrepro_container", NULL, PROV_RSA_FULL, 0);
	out("CryptAcquireContextA", "existing", "%s", r ? "ok" : Fmt("fail err=%08lx", ::GetLastError()).c_str());
	if (r)
		::CryptReleaseContext(pa, 0);
	r = ::CryptAcquireContextA(&pa, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT);
	out("CryptAcquireContextA", "verifycontext", "%s", r ? "ok" : Fmt("fail err=%08lx", ::GetLastError()).c_str());
	if (r)
		::CryptReleaseContext(pa, 0);
	r = ::CryptAcquireContextW(&p, name, NULL, 9999, 0);
	out("CryptAcquireContextW", "badtype", "%s", r ? "ok" : Fmt("fail err=%08lx", ::GetLastError()).c_str());
	::SetLastError(0);
	r = ::CryptReleaseContext(0, 0);
	out("CryptReleaseContext", "zero", "%s", B(r).c_str());
	r = ::CryptAcquireContextW(&p, name, NULL, PROV_RSA_FULL, CRYPT_DELETEKEYSET);
	out("CryptAcquireContextW", "deletekeyset", "%s", r ? "ok" : Fmt("fail err=%08lx", ::GetLastError()).c_str());
}

TEST(crypt_CertStoreAndEncrypt)
{
	const wchar_t *name = L"symrepro_container2";
	HCRYPTPROV prov = 0;
	if (!::CryptAcquireContextW(&prov, name, NULL, PROV_RSA_FULL, 0))
		::CryptAcquireContextW(&prov, name, NULL, PROV_RSA_FULL, CRYPT_NEWKEYSET);
	HCERTSTORE st = ::CertOpenSystemStoreW(prov, L"AddressBook");
	out("CertOpenSystemStoreW", "addressbook", "%s", st ? "ok" : Fmt("NULL err=%08lx", ::GetLastError()).c_str());
	if (!st)
		return;
	// Fixture: put the test certificate into the store.
	PCCERT_CONTEXT added = NULL;
	BOOL r = ::CertAddEncodedCertificateToStore(st, X509_ASN_ENCODING, kCertDer, sizeof kCertDer, CERT_STORE_ADD_REPLACE_EXISTING, &added);
	out("CertOpenSystemStoreW", "fixture.add", "%s", B(r).c_str());
	if (added)
		::CertFreeCertificateContext(added);
	// SendMail.cpp:93
	for (const wchar_t *subj : {L"symrepro test", L"SYMREPRO TEST", L"repro te", L"symrepro", L"nosuchsubject_sr", L""}) {
		::SetLastError(0);
		PCCERT_CONTEXT c = ::CertFindCertificateInStore(st, PKCS_7_ASN_ENCODING | X509_ASN_ENCODING, 0, CERT_FIND_SUBJECT_STR, subj, NULL);
		out("CertFindCertificateInStore", Q(subj).c_str(), "%s", c ? "found" : Fmt("NULL err=%08lx", ::GetLastError()).c_str());
		if (c)
			::CertFreeCertificateContext(c);
	}
	PCCERT_CONTEXT c = ::CertFindCertificateInStore(st, PKCS_7_ASN_ENCODING | X509_ASN_ENCODING, 0, CERT_FIND_SUBJECT_STR, L"symrepro test", NULL);
	if (c) {
		// LogCertificate
		wchar_t s[512];
		DWORD n = ::CertNameToStrW(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, &c->pCertInfo->Subject, CERT_X500_NAME_STR, s, _countof(s));
		out("CertNameToStrW", "subject.x500", "n=%lu %s", n, Q(s).c_str());
		n = ::CertNameToStrW(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, &c->pCertInfo->Issuer, CERT_X500_NAME_STR, s, _countof(s));
		out("CertNameToStrW", "issuer.x500", "n=%lu %s", n, Q(s).c_str());
		n = ::CertNameToStrW(X509_ASN_ENCODING, &c->pCertInfo->Subject, CERT_SIMPLE_NAME_STR, s, _countof(s));
		out("CertNameToStrW", "subject.simple", "n=%lu %s", n, Q(s).c_str());
		n = ::CertNameToStrW(X509_ASN_ENCODING, &c->pCertInfo->Subject, CERT_X500_NAME_STR | CERT_NAME_STR_REVERSE_FLAG, s, _countof(s));
		out("CertNameToStrW", "subject.reverse", "n=%lu %s", n, Q(s).c_str());
		n = ::CertNameToStrW(X509_ASN_ENCODING, &c->pCertInfo->Subject, CERT_X500_NAME_STR, NULL, 0);
		out("CertNameToStrW", "sizequery", "n=%lu", n);
		n = ::CertNameToStrW(X509_ASN_ENCODING, &c->pCertInfo->Subject, CERT_X500_NAME_STR, s, 5);
		out("CertNameToStrW", "small", "n=%lu %s", n, Q(s).c_str());
		n = ::CertGetNameStringW(c, CERT_NAME_SIMPLE_DISPLAY_TYPE, CERT_NAME_DISABLE_IE4_UTF8_FLAG, (void*)szOID_COMMON_NAME, s, _countof(s));
		out("CertGetNameStringW", "simpledisplay", "n=%lu %s", n, Q(s).c_str());
		n = ::CertGetNameStringW(c, CERT_NAME_ATTR_TYPE, 0, (void*)szOID_ORGANIZATION_NAME, s, _countof(s));
		out("CertGetNameStringW", "attr.org", "n=%lu %s", n, Q(s).c_str());
		n = ::CertGetNameStringW(c, CERT_NAME_EMAIL_TYPE, 0, NULL, s, _countof(s));
		out("CertGetNameStringW", "email.none", "n=%lu %s", n, Q(s).c_str());
		n = ::CertGetNameStringW(c, CERT_NAME_SIMPLE_DISPLAY_TYPE, CERT_NAME_ISSUER_FLAG, NULL, s, _countof(s));
		out("CertGetNameStringW", "issuer", "n=%lu %s", n, Q(s).c_str());
		BYTE md5[16];
		DWORD cb = sizeof md5;
		r = ::CertGetCertificateContextProperty(c, CERT_MD5_HASH_PROP_ID, md5, &cb);
		out("CertGetCertificateContextProperty", "md5", "%s cb=%lu %s", B(r).c_str(), cb, r ? Hex(md5, cb).c_str() : "");
		BYTE sha1[20];
		cb = sizeof sha1;
		r = ::CertGetCertificateContextProperty(c, CERT_SHA1_HASH_PROP_ID, sha1, &cb);
		out("CertGetCertificateContextProperty", "sha1", "%s cb=%lu %s", B(r).c_str(), cb, r ? Hex(sha1, cb).c_str() : "");
		cb = 4;
		r = ::CertGetCertificateContextProperty(c, CERT_SHA1_HASH_PROP_ID, sha1, &cb);
		out("CertGetCertificateContextProperty", "sha1.small", "%s cb=%lu", B(r).c_str(), cb);
		cb = 0;
		r = ::CertGetCertificateContextProperty(c, CERT_SHA1_HASH_PROP_ID, NULL, &cb);
		out("CertGetCertificateContextProperty", "sha1.query", "%s cb=%lu", B(r).c_str(), cb);
		cb = sizeof sha1;
		r = ::CertGetCertificateContextProperty(c, CERT_KEY_PROV_INFO_PROP_ID, sha1, &cb);
		out("CertGetCertificateContextProperty", "keyprovinfo.absent", "%s", B(r).c_str());
		out("CertNameToStrW", "serial", "%s", Hex(c->pCertInfo->SerialNumber.pbData, c->pCertInfo->SerialNumber.cbData).c_str());
		// SendMail.cpp:100-115 - 3DES enveloped message, size query then encrypt.
		CRYPT_ALGORITHM_IDENTIFIER alg = {};
		alg.pszObjId = const_cast<LPSTR>(szOID_RSA_DES_EDE3_CBC);
		CRYPT_ENCRYPT_MESSAGE_PARA ep = {};
		ep.cbSize = sizeof ep;
		ep.dwMsgEncodingType = PKCS_7_ASN_ENCODING | X509_ASN_ENCODING;
		ep.hCryptProv = prov;
		ep.ContentEncryptionAlgorithm = alg;
		const char msg[] = "Subject: test\r\n\r\nThe quick brown fox jumps over the lazy dog.\r\n";
		DWORD sz = 0;
		r = ::CryptEncryptMessage(&ep, 1, &c, (const BYTE*)msg, sizeof msg - 1, NULL, &sz);
		out("CryptEncryptMessage", "sizequery", "%s", r ? "ok" : Fmt("fail err=%08lx", ::GetLastError()).c_str());
		std::vector<BYTE> blob(sz ? sz : 1);
		DWORD sz2 = sz;
		r = ::CryptEncryptMessage(&ep, 1, &c, (const BYTE*)msg, sizeof msg - 1, blob.data(), &sz2);
		out("CryptEncryptMessage", "encrypt", "%s fits=%d", r ? "ok" : Fmt("fail err=%08lx", ::GetLastError()).c_str(), sz2 <= sz);
		out("CryptEncryptMessage", "~size", "query=%lu actual=%lu", sz, sz2);
		// Decrypt with the private key from the PKCS#12 fixture, to prove the
		// envelope is a real one.
		CRYPT_DATA_BLOB pfx = {sizeof kCertPfx, const_cast<BYTE*>(kCertPfx)};
		HCERTSTORE ks = ::PFXImportCertStore(&pfx, L"symrepro", CRYPT_USER_KEYSET);
		out("CryptEncryptMessage", "fixture.pfx", "%s", ks ? "ok" : Fmt("NULL err=%08lx", ::GetLastError()).c_str());
		if (ks && r) {
			CRYPT_DECRYPT_MESSAGE_PARA dp = {};
			dp.cbSize = sizeof dp;
			dp.dwMsgAndCertEncodingType = PKCS_7_ASN_ENCODING | X509_ASN_ENCODING;
			dp.cCertStore = 1;
			dp.rghCertStore = &ks;
			std::vector<BYTE> plain(sz2 + 16);
			DWORD pl = (DWORD)plain.size();
			BOOL d = ::CryptDecryptMessage(&dp, blob.data(), sz2, plain.data(), &pl, NULL);
			out("CryptEncryptMessage", "roundtrip", "%s same=%d", d ? "ok" : Fmt("fail err=%08lx", ::GetLastError()).c_str(),
				d && pl == sizeof msg - 1 && !memcmp(plain.data(), msg, pl));
		}
		if (ks)
			::CertCloseStore(ks, 0);
		// RC4 (weak) and an unknown OID.
		for (const char *oid : {szOID_RSA_RC2CBC, szOID_RSA_RC4, szOID_NIST_AES256_CBC, "1.2.3.4.5"}) {
			ep.ContentEncryptionAlgorithm.pszObjId = const_cast<LPSTR>(oid);
			sz = 0;
			r = FALSE;
			DWORD e = 0;
			DWORD ex = Guard([&] { r = ::CryptEncryptMessage(&ep, 1, &c, (const BYTE*)msg, sizeof msg - 1, NULL, &sz); e = ::GetLastError(); });
			out("CryptEncryptMessage", Fmt("oid.%s", oid).c_str(), "%s%s", r ? "ok" : Fmt("fail err=%08lx", e).c_str(), GuardStr(ex).c_str());
		}
		out("CertFreeCertificateContext", "found", "%s", B(::CertFreeCertificateContext(c)).c_str());
	}
	out("CertFreeCertificateContext", "null", "%s", B(::CertFreeCertificateContext(NULL)).c_str());
	// Clean the fixture out of the store.
	PCCERT_CONTEXT d = ::CertFindCertificateInStore(st, X509_ASN_ENCODING, 0, CERT_FIND_SUBJECT_STR, L"symrepro test", NULL);
	if (d)
		::CertDeleteCertificateFromStore(d);
	out("CertCloseStore", "addressbook", "%s", B(::CertCloseStore(st, 0)).c_str());
	// Other system stores eMule could be pointed at.
	for (const wchar_t *n : {L"MY", L"ROOT", L"CA", L"NoSuchStore_sr"}) {
		HCERTSTORE s = ::CertOpenSystemStoreW(0, n);
		int count = 0;
		for (PCCERT_CONTEXT x = NULL; s && (x = ::CertEnumCertificatesInStore(s, x)) != NULL;)
			++count;
		out("CertOpenSystemStoreW", Narrow(n).c_str(), "%s", s ? "ok" : Fmt("NULL err=%08lx", ::GetLastError()).c_str());
		out("CertOpenSystemStoreW", ("~" + Narrow(n) + ".count").c_str(), "%d", count);
		if (s)
			::CertCloseStore(s, 0);
	}
	::CryptReleaseContext(prov, 0);
	::CryptAcquireContextW(&prov, name, NULL, PROV_RSA_FULL, CRYPT_DELETEKEYSET);
}
