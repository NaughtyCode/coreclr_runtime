// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Collections.Generic;
using System.Security.Cryptography.Tests;
using System.Security.Cryptography.Dsa.Tests;
using System.Security.Cryptography.SLHDsa.Tests;
using System.Security.Cryptography.X509Certificates.Tests.CertificateCreation;
using System.Threading;
using Test.Cryptography;
using Xunit;
using Xunit.Abstractions;

namespace System.Security.Cryptography.X509Certificates.Tests
{
    [SkipOnPlatform(TestPlatforms.Browser, "Browser doesn't support X.509 certificates")]
    public class CertTests
    {
        private const string PrivateKeySectionHeader = "[Private Key]";
        private const string PublicKeySectionHeader = "[Public Key]";

        private readonly ITestOutputHelper _log;

        public CertTests(ITestOutputHelper output)
        {
            _log = output;
        }

        [Fact]
        public static void PrivateKey_FromCertificate_CanExportPrivate_ECDsa()
        {
            using (ECDsa ca = ECDsa.Create(ECCurve.NamedCurves.nistP256))
            {
                CertificateRequest req = new("CN=potatos", ca, HashAlgorithmName.SHA256);

                using (X509Certificate2 cert = req.CreateSelfSigned(DateTimeOffset.Now, DateTimeOffset.Now.AddDays(3)))
                using (ECDsa certKey = cert.GetECDsaPrivateKey())
                {
                    ECParameters certParameters = certKey.ExportParameters(true);
                    ECParameters originalParameters = ca.ExportParameters(true);
                    AssertExtensions.SequenceEqual(originalParameters.D, certParameters.D);
                }
            }
        }

        [Fact]
        public static void PrivateKey_FromCertificate_CanExportPrivate_RSA()
        {
            using (RSA ca = RSA.Create(2048))
            {
                CertificateRequest req = new("CN=potatos", ca, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);

                using (X509Certificate2 cert = req.CreateSelfSigned(DateTimeOffset.Now, DateTimeOffset.Now.AddDays(3)))
                using (RSA certKey = cert.GetRSAPrivateKey())
                {
                    RSAParameters certParameters = certKey.ExportParameters(true);
                    RSAParameters originalParameters = ca.ExportParameters(true);
                    AssertExtensions.SequenceEqual(originalParameters.P, certParameters.P);
                    AssertExtensions.SequenceEqual(originalParameters.Q, certParameters.Q);
                }
            }
        }

        [Fact]
        [SkipOnPlatform(PlatformSupport.MobileAppleCrypto, "DSA is not available")]
        public static void PrivateKey_FromCertificate_CanExportPrivate_DSA()
        {
            DSAParameters originalParameters = DSATestData.GetDSA1024Params();

            using (DSA ca = DSA.Create())
            {
                ca.ImportParameters(originalParameters);
                DSAX509SignatureGenerator gen = new DSAX509SignatureGenerator(ca);
                X500DistinguishedName dn = new X500DistinguishedName("CN=potatos");

                CertificateRequest req = new CertificateRequest(
                    dn,
                    gen.PublicKey,
                    HashAlgorithmName.SHA1);

                using (X509Certificate2 cert = req.Create(dn, gen, DateTimeOffset.Now, DateTimeOffset.Now.AddDays(3), [1, 2, 3]))
                using (X509Certificate2 certWithKey = cert.CopyWithPrivateKey(ca))
                using (DSA certKey = certWithKey.GetDSAPrivateKey())
                {
                    DSAParameters certParameters = certKey.ExportParameters(true);
                    AssertExtensions.SequenceEqual(originalParameters.X, certParameters.X);
                }
            }
        }

        [Fact]
        public static void PrivateKey_FromCertificate_CanExportPrivate_ECDiffieHellman()
        {
            using (ECDsa ca = ECDsa.Create(ECCurve.NamedCurves.nistP256))
            using (ECDiffieHellman ecdh = ECDiffieHellman.Create(ECCurve.NamedCurves.nistP256))
            {
                CertificateRequest issuerRequest = new CertificateRequest(
                    new X500DistinguishedName("CN=root"),
                    ca,
                    HashAlgorithmName.SHA256);

                issuerRequest.CertificateExtensions.Add(
                    new X509BasicConstraintsExtension(true, false, 0, true));

                CertificateRequest request = new CertificateRequest(
                    new X500DistinguishedName("CN=potato"),
                    new PublicKey(ecdh),
                    HashAlgorithmName.SHA256);

                request.CertificateExtensions.Add(
                    new X509BasicConstraintsExtension(false, false, 0, true));
                request.CertificateExtensions.Add(
                    new X509KeyUsageExtension(X509KeyUsageFlags.KeyAgreement, true));

                DateTimeOffset notBefore = DateTimeOffset.UtcNow;
                DateTimeOffset notAfter = notBefore.AddDays(30);
                byte[] serial = [1, 2, 3, 4, 5, 6, 7, 8];

                using (X509Certificate2 issuer = issuerRequest.CreateSelfSigned(notBefore, notAfter))
                using (X509Certificate2 cert = request.Create(issuer, notBefore, notAfter, serial))
                using (X509Certificate2 certWithKey = cert.CopyWithPrivateKey(ecdh))
                using (ECDiffieHellman certKey = certWithKey.GetECDiffieHellmanPrivateKey())
                {
                    ECParameters certParameters = certKey.ExportParameters(true);
                    ECParameters originalParameters = ecdh.ExportParameters(true);
                    AssertExtensions.SequenceEqual(originalParameters.D, certParameters.D);
                }
            }
        }

        [ConditionalFact(typeof(PlatformSupport), nameof(PlatformSupport.IsPqcMLKemX509Supported))]
        public static void PrivateKey_FromCertificate_CanExportPrivate_MLKem()
        {
            using (X509Certificate2 cert = X509Certificate2.CreateFromPem(MLKemTestData.IetfMlKem512CertificatePem))
            using (MLKem key = MLKem.ImportPkcs8PrivateKey(MLKemTestData.IetfMlKem512PrivateKeySeed))
            using (X509Certificate2 certWithKey = cert.CopyWithPrivateKey(key))
            using (MLKem certKey = certWithKey.GetMLKemPrivateKey())
            {
                Assert.NotNull(certKey);
                byte[] expectedKey = MLKemTestData.IetfMlKem512PrivateKeyDecapsulationKey;
                AssertExtensions.SequenceEqual(expectedKey, certKey.ExportDecapsulationKey());
            }
        }

        [ConditionalFact(typeof(MLDsa), nameof(MLDsa.IsSupported))]
        public static void PrivateKey_FromCertificate_CanExportPrivate_MLDsa()
        {
            string pem = PemEncoding.WriteString("CERTIFICATE", MLDsaTestsData.IetfMLDsa44.Certificate);
            using (X509Certificate2 cert = X509Certificate2.CreateFromPem(pem))
            using (MLDsa key = MLDsa.ImportPkcs8PrivateKey(MLDsaTestsData.IetfMLDsa44.Pkcs8PrivateKey_Seed))
            using (X509Certificate2 certWithKey = cert.CopyWithPrivateKey(key))
            using (MLDsa certKey = certWithKey.GetMLDsaPrivateKey())
            {
                Assert.NotNull(certKey);
                byte[] expectedKey = MLDsaTestsData.IetfMLDsa44.SecretKey;
                byte[] actualKey = certKey.ExportMLDsaSecretKey();
                AssertExtensions.SequenceEqual(expectedKey, actualKey);
            }
        }

        [ConditionalFact(typeof(SlhDsa), nameof(SlhDsa.IsSupported))]
        public static void PrivateKey_FromCertificate_CanExportPrivate_SlhDsa()
        {
            string pem = PemEncoding.WriteString("CERTIFICATE", SlhDsaTestData.IetfSlhDsaSha2_128sCertificate);
            using (X509Certificate2 cert = X509Certificate2.CreateFromPem(pem))
            using (SlhDsa key = SlhDsa.ImportPkcs8PrivateKey(SlhDsaTestData.IetfSlhDsaSha2_128sPrivateKeyPkcs8))
            using (X509Certificate2 certWithKey = cert.CopyWithPrivateKey(key))
            using (SlhDsa certKey = certWithKey.GetSlhDsaPrivateKey())
            {
                Assert.NotNull(certKey);
                byte[] expectedKey = SlhDsaTestData.IetfSlhDsaSha2_128sPrivateKeyValue;
                AssertExtensions.SequenceEqual(expectedKey, certKey.ExportSlhDsaPrivateKey());
            }
        }

        [Fact]
        public static void PublicPrivateKey_IndependentLifetimes_ECDsa()
        {
            X509Certificate2 loaded;

            using (ECDsa ca = ECDsa.Create(ECCurve.NamedCurves.nistP256))
            {
                CertificateRequest req = new("CN=potatos", ca, HashAlgorithmName.SHA256);

                using (X509Certificate2 cert = req.CreateSelfSigned(DateTimeOffset.Now, DateTimeOffset.Now.AddDays(3)))
                {
                    loaded = new X509Certificate2(cert.Export(X509ContentType.Pkcs12, "carrots"), "carrots");
                }
            }

            using (ECDsa verifyKey = loaded.GetECDsaPublicKey())
            {
                byte[] signature;
                byte[] data = RandomNumberGenerator.GetBytes(32);

                using (ECDsa signingKey = loaded.GetECDsaPrivateKey())
                {
                    loaded.Dispose();
                    signature = signingKey.SignHash(data);
                }

                Assert.True(verifyKey.VerifyHash(data, signature), nameof(verifyKey.VerifyHash));
            }
        }

        [Fact]
        public static void PublicPrivateKey_IndependentLifetimes_ECDiffieHellman()
        {
            X509Certificate2 loaded;

            using (ECDsa ca = ECDsa.Create(ECCurve.NamedCurves.nistP256))
            using (ECDiffieHellman ecdh = ECDiffieHellman.Create(ECCurve.NamedCurves.nistP256))
            {
                CertificateRequest issuerRequest = new CertificateRequest(
                    new X500DistinguishedName("CN=root"),
                    ca,
                    HashAlgorithmName.SHA256);

                issuerRequest.CertificateExtensions.Add(
                    new X509BasicConstraintsExtension(true, false, 0, true));

                CertificateRequest request = new CertificateRequest(
                    new X500DistinguishedName("CN=potato"),
                    new PublicKey(ecdh),
                    HashAlgorithmName.SHA256);

                request.CertificateExtensions.Add(
                    new X509BasicConstraintsExtension(false, false, 0, true));
                request.CertificateExtensions.Add(
                    new X509KeyUsageExtension(X509KeyUsageFlags.KeyAgreement, true));

                DateTimeOffset notBefore = DateTimeOffset.UtcNow;
                DateTimeOffset notAfter = notBefore.AddDays(30);
                byte[] serial = [1, 2, 3, 4, 5, 6, 7, 8];

                using (X509Certificate2 issuer = issuerRequest.CreateSelfSigned(notBefore, notAfter))
                using (X509Certificate2 cert = request.Create(issuer, notBefore, notAfter, serial))
                using (X509Certificate2 certWithKey = cert.CopyWithPrivateKey(ecdh))
                {
                    loaded = new X509Certificate2(certWithKey.Export(X509ContentType.Pkcs12, "carrots"), "carrots");;
                }
            }

            using (ECDiffieHellman partyB = ECDiffieHellman.Create(ECCurve.NamedCurves.nistP256))
            using (ECDiffieHellman partyAPrivateKey = loaded.GetECDiffieHellmanPrivateKey())
            using (ECDiffieHellman partyAPublicKey = loaded.GetECDiffieHellmanPublicKey())
            {
                loaded.Dispose();
                byte[] derivedB = partyB.DeriveKeyFromHash(partyAPublicKey.PublicKey, HashAlgorithmName.SHA256, null, null);
                byte[] derivedA = partyAPrivateKey.DeriveKeyFromHash(partyB.PublicKey, HashAlgorithmName.SHA256, null, null);
                Assert.Equal(derivedB, derivedA);
            }
        }

        [Fact]
        public static void PublicPrivateKey_IndependentLifetimes_RSA()
        {
            X509Certificate2 loaded;

            using (RSA ca = RSA.Create(2048))
            {
                CertificateRequest req = new("CN=potatos", ca, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);

                using (X509Certificate2 cert = req.CreateSelfSigned(DateTimeOffset.Now, DateTimeOffset.Now.AddDays(3)))
                {
                    loaded = new X509Certificate2(cert.Export(X509ContentType.Pkcs12, "carrots"), "carrots");
                }
            }

            using (RSA verifyKey = loaded.GetRSAPublicKey())
            {
                byte[] signature;
                byte[] data = RandomNumberGenerator.GetBytes(32);

                using (RSA signingKey = loaded.GetRSAPrivateKey())
                {
                    loaded.Dispose();
                    signature = signingKey.SignHash(data, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
                }

                Assert.True(verifyKey.VerifyHash(data, signature, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1), nameof(verifyKey.VerifyHash));
            }
        }

        [Fact]
        [SkipOnPlatform(PlatformSupport.MobileAppleCrypto, "DSA is not available")]
        public static void PublicPrivateKey_IndependentLifetimes_DSA()
        {
            X509Certificate2 loaded;

            using (DSA ca = DSA.Create())
            {
                ca.ImportParameters(DSATestData.GetDSA1024Params());
                DSAX509SignatureGenerator gen = new DSAX509SignatureGenerator(ca);
                X500DistinguishedName dn = new X500DistinguishedName("CN=potatos");

                CertificateRequest req = new CertificateRequest(
                    dn,
                    gen.PublicKey,
                    HashAlgorithmName.SHA1);

                using (X509Certificate2 cert = req.Create(dn, gen, DateTimeOffset.Now, DateTimeOffset.Now.AddDays(3), new byte[] { 1, 2, 3 }))
                using (X509Certificate2 certWithKey = cert.CopyWithPrivateKey(ca))
                {

                    loaded = new X509Certificate2(certWithKey.Export(X509ContentType.Pkcs12, "carrots"), "carrots");
                }
            }

            using (DSA verifyKey = loaded.GetDSAPublicKey())
            {
                byte[] signature;
                byte[] data = RandomNumberGenerator.GetBytes(20);

                using (DSA signingKey = loaded.GetDSAPrivateKey())
                {
                    loaded.Dispose();
                    signature = signingKey.CreateSignature(data);
                }

                Assert.True(verifyKey.VerifySignature(data, signature), nameof(verifyKey.VerifySignature));
            }
        }

        [Fact]
        public static void RaceDisposeAndKeyAccess()
        {
            using RSA rsa = RSA.Create();
            CertificateRequest req = new CertificateRequest("CN=potato", rsa, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
            using X509Certificate2 cert = req.CreateSelfSigned(DateTimeOffset.Now, DateTimeOffset.Now);

            for (int i = 0; i < 100; i++)
            {
                X509Certificate2 w = new X509Certificate2(cert.RawData.AsSpan());
                X509Certificate2 y = w.CopyWithPrivateKey(rsa);
                w.Dispose();

                Thread t1 = new Thread(cert => {
                    Thread.Sleep(Random.Shared.Next(0, 20));
                    X509Certificate2 c = (X509Certificate2)cert!;
                    c.Dispose();
                    GC.Collect();
                });

                Thread t2 = new Thread(cert => {
                    Thread.Sleep(Random.Shared.Next(0, 20));
                    X509Certificate2 c = (X509Certificate2)cert!;

                    try
                    {
                        c.GetRSAPrivateKey()!.ExportParameters(false);
                    }
                    catch
                    {
                        // don't care about managed exceptions.
                    }
                });

                t1.Start(y);
                t2.Start(y);
                t1.Join();
                t2.Join();
            }
        }

        [Fact]
        public static void RaceUseAndDisposeDoesNotCrash()
        {
            X509Certificate2 cert = new X509Certificate2(TestFiles.MicrosoftRootCertFile);

            Thread subjThread = new Thread(static state => {
                X509Certificate2 c = (X509Certificate2)state;

                try
                {
                    _ = c.Subject;
                }
                catch
                {
                    // managed exceptions are okay, we are looking for runtime crashes.
                }
            });

            Thread disposeThread = new Thread(static state => {
                ((X509Certificate2)state).Dispose();
            });

            subjThread.Start(cert);
            disposeThread.Start(cert);
            disposeThread.Join();
            subjThread.Join();
        }

        [Fact]
        public static void X509CertTest()
        {
            string certSubject = @"CN=Microsoft Corporate Root Authority, OU=ITG, O=Microsoft, L=Redmond, S=WA, C=US, E=pkit@microsoft.com";
            string certSubjectObsolete = @"E=pkit@microsoft.com, C=US, S=WA, L=Redmond, O=Microsoft, OU=ITG, CN=Microsoft Corporate Root Authority";

            using (X509Certificate cert = new X509Certificate(TestFiles.MicrosoftRootCertFile))
            {
                Assert.Equal(certSubject, cert.Subject);
                Assert.Equal(certSubject, cert.Issuer);
#pragma warning disable CS0618 // Type or member is obsolete
                Assert.Equal(certSubjectObsolete, cert.GetName());
                Assert.Equal(certSubjectObsolete, cert.GetIssuerName());
#pragma warning restore CS0618

                byte[] serial1 = cert.GetSerialNumber();
                byte[] serial2 = cert.GetSerialNumber();
                Assert.NotSame(serial1, serial2);
                AssertExtensions.SequenceEqual(serial1, serial2);

                // Big-endian value
                byte[] expectedSerial = "2A98A8770374E7B34195EBE04D9B17F6".HexToByteArray();
                AssertExtensions.SequenceEqual(expectedSerial, cert.SerialNumberBytes.Span);

                // GetSerialNumber() returns in little-endian order.
                Array.Reverse(expectedSerial);
                AssertExtensions.SequenceEqual(expectedSerial, serial1);

                Assert.Equal("1.2.840.113549.1.1.1", cert.GetKeyAlgorithm());

                int pklen = cert.GetPublicKey().Length;
                Assert.Equal(270, pklen);

                byte[] publicKey = new byte[pklen];
                Buffer.BlockCopy(cert.GetPublicKey(), 0,
                                     publicKey, 0,
                                     pklen);

                Assert.Equal(0x30, publicKey[0]);
                Assert.Equal(0xB6, publicKey[9]);
                Assert.Equal(1, publicKey[pklen - 1]);
            }
        }

        [Fact]
        public static void X509Cert2Test()
        {
            string certName = @"E=admin@digsigtrust.com, CN=ABA.ECOM Root CA, O=""ABA.ECOM, INC."", L=Washington, S=DC, C=US";

            DateTime notBefore = new DateTime(1999, 7, 12, 17, 33, 53, DateTimeKind.Utc).ToLocalTime();
            DateTime notAfter = new DateTime(2009, 7, 9, 17, 33, 53, DateTimeKind.Utc).ToLocalTime();

            using (X509Certificate2 cert2 = new X509Certificate2(TestFiles.TestCertFile))
            {
                Assert.Equal(certName, cert2.IssuerName.Name);
                Assert.Equal(certName, cert2.SubjectName.Name);

                Assert.Equal("ABA.ECOM Root CA", cert2.GetNameInfo(X509NameType.DnsName, true));

                PublicKey pubKey = cert2.PublicKey;
                Assert.Equal("RSA", pubKey.Oid.FriendlyName);

                Assert.Equal(notAfter, cert2.NotAfter);
                Assert.Equal(notBefore, cert2.NotBefore);

                Assert.Equal(notAfter.ToString(), cert2.GetExpirationDateString());
                Assert.Equal(notBefore.ToString(), cert2.GetEffectiveDateString());

                Assert.Equal("00D01E4090000046520000000100000004", cert2.SerialNumber);
                Assert.Equal("1.2.840.113549.1.1.5", cert2.SignatureAlgorithm.Value);
                Assert.NotEmpty(cert2.SignatureAlgorithm.FriendlyName);
                Assert.Equal("7A74410FB0CD5C972A364B71BF031D88A6510E9E", cert2.Thumbprint);
                Assert.Equal(3, cert2.Version);
            }
        }

        [ActiveIssue("https://github.com/dotnet/runtime/issues/26213")]
        [ConditionalFact]
        [OuterLoop("May require using the network, to download CRLs and intermediates", ~TestPlatforms.Browser)]
        public void TestVerify()
        {
            bool success;

            using (var microsoftDotCom = new X509Certificate2(TestData.MicrosoftDotComLegacySslCertBytes))
            {
                // Fails because expired (NotAfter = 10/16/2016)
                Assert.False(microsoftDotCom.Verify(), "MicrosoftDotComLegacySslCertBytes");
            }

            using (var microsoftDotComIssuer = new X509Certificate2(TestData.MicrosoftDotComIssuerBytes))
            {
                // NotAfter=10/8/2024, 7:00:00 AM UTC
                success = microsoftDotComIssuer.Verify();
                if (!success)
                {
                    LogVerifyErrors(microsoftDotComIssuer, "MicrosoftDotComIssuerBytes");
                }

                Assert.True(success, "MicrosoftDotComIssuerBytes");
            }

            // High Sierra fails to build a chain for a self-signed certificate with revocation enabled.
            // https://github.com/dotnet/runtime/issues/22625
            if (PlatformDetection.IsNotOSX)
            {
                using (var microsoftDotComRoot = new X509Certificate2(TestData.MicrosoftDotComRootBytes))
                {
                    // NotAfter=7/17/2025
                    success = microsoftDotComRoot.Verify();
                    if (!success)
                    {
                        LogVerifyErrors(microsoftDotComRoot, "MicrosoftDotComRootBytes");
                    }
                    Assert.True(success, "MicrosoftDotComRootBytes");
                }
            }
        }

        private void LogVerifyErrors(X509Certificate2 cert, string testName)
        {
            // Emulate cert.Verify() implementation in order to capture and log errors.
            try
            {
                using (var chain = new X509Chain())
                {
                    if (!chain.Build(cert))
                    {
                        foreach (X509ChainStatus chainStatus in chain.ChainStatus)
                        {
                            _log.WriteLine($"X509Certificate2.Verify error: {testName}, {chainStatus.Status}, {chainStatus.StatusInformation}");
                        }
                    }
                    else
                    {
                        _log.WriteLine($"X509Certificate2.Verify expected error; received none: {testName}");
                    }
                }
            }
            catch (Exception e)
            {
                _log.WriteLine($"X509Certificate2.Verify exception: {testName}, {e}");
            }
        }

        [Fact]
        public static void X509CertEmptyToString()
        {
            using (var c = new X509Certificate())
            {
                string expectedResult = "System.Security.Cryptography.X509Certificates.X509Certificate";
                Assert.Equal(expectedResult, c.ToString());
                Assert.Equal(expectedResult, c.ToString(false));
                Assert.Equal(expectedResult, c.ToString(true));
            }
        }

        [Fact]
        public static void X509Cert2EmptyToString()
        {
            using (var c2 = new X509Certificate2())
            {
                string expectedResult = "System.Security.Cryptography.X509Certificates.X509Certificate2";
                Assert.Equal(expectedResult, c2.ToString());
                Assert.Equal(expectedResult, c2.ToString(false));
                Assert.Equal(expectedResult, c2.ToString(true));
            }
        }

        [Fact]
        public static void X509Cert2ToStringVerbose()
        {
            using (X509Store store = new X509Store("My", StoreLocation.CurrentUser))
            {
                store.Open(OpenFlags.ReadOnly);

                foreach (X509Certificate2 c in store.Certificates)
                {
                    Assert.False(string.IsNullOrWhiteSpace(c.ToString(true)));
                    c.Dispose();
                }
            }
        }

        [Theory]
        [MemberData(nameof(StorageFlags))]
        public static void X509Certificate2ToStringVerbose_WithPrivateKey(X509KeyStorageFlags keyStorageFlags)
        {
            using (var cert = new X509Certificate2(TestData.PfxData, TestData.PfxDataPassword, keyStorageFlags))
            {
                string certToString = cert.ToString(true);
                Assert.Contains(PrivateKeySectionHeader, certToString);
                Assert.Contains(PublicKeySectionHeader, certToString);
            }
        }

        [Theory]
        [MemberData(nameof(StorageFlags))]
        public static void X509Certificate2ToStringVerbose_WithPrivateKey_FromSpans(X509KeyStorageFlags keyStorageFlags)
        {
            Span<char> pwTmp = stackalloc char[30];
            pwTmp.Fill('Z');
            TestData.PfxDataPassword.AsSpan().CopyTo(pwTmp);
            ReadOnlySpan<char> pw = pwTmp.Slice(0, TestData.PfxDataPassword.Length);

            using (var cert = new X509Certificate2(TestData.PfxData.AsSpan(), pw, keyStorageFlags))
            {
                string certToString = cert.ToString(true);
                Assert.Contains(PrivateKeySectionHeader, certToString);
                Assert.Contains(PublicKeySectionHeader, certToString);
            }
        }

        [Fact]
        public static void X509Certificate2ToStringVerbose_NoPrivateKey()
        {
            using (var cert = new X509Certificate2(TestData.MsCertificatePemBytes))
            {
                string certToString = cert.ToString(true);
                Assert.DoesNotContain(PrivateKeySectionHeader, certToString);
                Assert.Contains(PublicKeySectionHeader, certToString);
            }
        }

        [Fact]
        public static void X509Certificate2ToStringVerbose_NoPrivateKey_FromSpan()
        {
            using (var cert = new X509Certificate2(TestData.MsCertificatePemBytes.AsSpan()))
            {
                string certToString = cert.ToString(true);
                Assert.DoesNotContain(PrivateKeySectionHeader, certToString);
                Assert.Contains(PublicKeySectionHeader, certToString);
            }
        }

        [Fact]
        public static void X509Cert2CreateFromEmptyPfx()
        {
            Assert.ThrowsAny<CryptographicException>(() => new X509Certificate2(TestData.EmptyPfx));
        }

        [Fact]
        public static void X509Cert2CreateFromPfxFile()
        {
            using (X509Certificate2 cert2 = new X509Certificate2(TestFiles.DummyTcpServerPfxFile))
            {
                // OID=RSA Encryption
                Assert.Equal("1.2.840.113549.1.1.1", cert2.GetKeyAlgorithm());
            }
        }

        [Fact]
        public static void X509Cert2CreateFromPfxWithPassword()
        {
            using (X509Certificate2 cert2 = new X509Certificate2(TestFiles.ChainPfxFile, TestData.ChainPfxPassword))
            {
                // OID=RSA Encryption
                Assert.Equal("1.2.840.113549.1.1.1", cert2.GetKeyAlgorithm());
            }
        }

        [Fact]
        public static void X509Cert2CreateFromPfxWithSpanPassword()
        {
            Span<char> pw = stackalloc char[] { 't', 'e', 's', 't' };

            using (X509Certificate2 cert2 = new X509Certificate2(TestFiles.ChainPfxFile, pw))
            {
                // OID=RSA Encryption
                Assert.Equal("1.2.840.113549.1.1.1", cert2.GetKeyAlgorithm());
            }
        }

        [Fact]
        public static void X509Certificate2FromPkcs7DerFile()
        {
            Assert.ThrowsAny<CryptographicException>(() => new X509Certificate2(TestFiles.Pkcs7SingleDerFile));
        }

        [Fact]
        public static void X509Certificate2FromPkcs7PemFile()
        {
            Assert.ThrowsAny<CryptographicException>(() => new X509Certificate2(TestFiles.Pkcs7SinglePemFile));
        }

        [Fact]
        public static void X509Certificate2FromPkcs7DerBlob()
        {
            Assert.ThrowsAny<CryptographicException>(() => new X509Certificate2(TestData.Pkcs7SingleDerBytes));
        }

        [Fact]
        public static void X509Certificate2FromPkcs7PemBlob()
        {
            Assert.ThrowsAny<CryptographicException>(() => new X509Certificate2(TestData.Pkcs7SinglePemBytes));
        }

        [Fact]
        public static void UseAfterDispose()
        {
            using (X509Certificate2 c = new X509Certificate2(TestData.MsCertificate))
            {
                IntPtr h = c.Handle;

                // Do a couple of things that would only be true on a valid certificate, as a precondition.
                Assert.NotEqual(IntPtr.Zero, h);
                byte[] actualThumbprint = c.GetCertHash();

                c.Dispose();

                // For compat reasons, Dispose() acts like the now-defunct Reset() method rather than
                // causing ObjectDisposedExceptions.
                h = c.Handle;
                Assert.Equal(IntPtr.Zero, h);

                // State held on X509Certificate
                Assert.ThrowsAny<CryptographicException>(() => c.GetCertHash());
                Assert.ThrowsAny<CryptographicException>(() => c.GetCertHashString());
                Assert.ThrowsAny<CryptographicException>(() => c.GetCertHash(HashAlgorithmName.SHA256));
                Assert.ThrowsAny<CryptographicException>(() => c.GetCertHashString(HashAlgorithmName.SHA256));
                Assert.ThrowsAny<CryptographicException>(() => c.GetKeyAlgorithm());
                Assert.ThrowsAny<CryptographicException>(() => c.GetKeyAlgorithmParameters());
                Assert.ThrowsAny<CryptographicException>(() => c.GetKeyAlgorithmParametersString());
                Assert.ThrowsAny<CryptographicException>(() => c.GetPublicKey());
                Assert.ThrowsAny<CryptographicException>(() => c.GetSerialNumber());
                Assert.ThrowsAny<CryptographicException>(() => c.SerialNumberBytes);
                Assert.ThrowsAny<CryptographicException>(() => c.Issuer);
                Assert.ThrowsAny<CryptographicException>(() => c.Subject);
                Assert.ThrowsAny<CryptographicException>(() => c.NotBefore);
                Assert.ThrowsAny<CryptographicException>(() => c.NotAfter);

                Assert.ThrowsAny<CryptographicException>(
                    () => c.TryGetCertHash(HashAlgorithmName.SHA256, Array.Empty<byte>(), out _));

                // State held on X509Certificate2
                Assert.ThrowsAny<CryptographicException>(() => c.RawDataMemory);
                Assert.ThrowsAny<CryptographicException>(() => c.RawData);
                Assert.ThrowsAny<CryptographicException>(() => c.SignatureAlgorithm);
                Assert.ThrowsAny<CryptographicException>(() => c.Version);
                Assert.ThrowsAny<CryptographicException>(() => c.SubjectName);
                Assert.ThrowsAny<CryptographicException>(() => c.IssuerName);
                Assert.ThrowsAny<CryptographicException>(() => c.PublicKey);
                Assert.ThrowsAny<CryptographicException>(() => c.Extensions);
                Assert.ThrowsAny<CryptographicException>(() => c.PrivateKey);
            }
        }

        [Fact]
        public static void EmptyPkcs7ThrowsException()
        {
            Assert.ThrowsAny<CryptographicException>(() => new X509Certificate2(TestData.EmptyPkcs7));
        }

        [Fact]
        public static void ExportPublicKeyAsPkcs12()
        {
            using (X509Certificate2 publicOnly = new X509Certificate2(TestData.MsCertificate))
            {
                // Pre-condition: There's no private key
                Assert.False(publicOnly.HasPrivateKey);

                byte[] pkcs12Bytes = publicOnly.Export(X509ContentType.Pkcs12);

                // Read it back as a collection, there should be only one cert, and it should
                // be equal to the one we started with.
                using (ImportedCollection ic = Cert.Import(pkcs12Bytes, (string?)null, X509KeyStorageFlags.DefaultKeySet))
                {
                    X509Certificate2Collection fromPfx = ic.Collection;

                    Assert.Equal(1, fromPfx.Count);
                    Assert.Equal(publicOnly, fromPfx[0]);
                }
            }
        }

        [Fact]
        public static void X509Certificate2WithT61String()
        {
            string certSubject = @"E=mabaul@microsoft.com, OU=Engineering, O=Xamarin, S=Massachusetts, C=US, CN=test-server.local";

            using (var cert = new X509Certificate2(TestData.T61StringCertificate))
            {
                Assert.Equal(certSubject, cert.Subject);
                Assert.Equal(certSubject, cert.Issuer);

                Assert.Equal("9E7A5CCC9F951A8700", cert.GetSerialNumber().ByteArrayToHex());
                Assert.Equal("00871A959FCC5C7A9E", cert.SerialNumberBytes.ByteArrayToHex());
                Assert.Equal("1.2.840.113549.1.1.1", cert.GetKeyAlgorithm());

                Assert.Equal(74, cert.GetPublicKey().Length);

                Assert.Equal("test-server.local", cert.GetNameInfo(X509NameType.SimpleName, false));
                Assert.Equal("mabaul@microsoft.com", cert.GetNameInfo(X509NameType.EmailName, false));
            }
        }

        [Fact]
        [PlatformSpecific(TestPlatforms.Windows)]
        public static void SerializedCertDisposeDoesNotRemoveKeyFile()
        {
            using (X509Certificate2 fromPfx = new X509Certificate2(TestData.PfxData, TestData.PfxDataPassword))
            {
                Assert.True(fromPfx.HasPrivateKey, "fromPfx.HasPrivateKey - before");

                byte[] serializedCert = fromPfx.Export(X509ContentType.SerializedCert);

                using (X509Certificate2 fromSerialized = new X509Certificate2(serializedCert))
                {
                    Assert.True(fromSerialized.HasPrivateKey, "fromSerialized.HasPrivateKey");
                }

                using (RSA key = fromPfx.GetRSAPrivateKey())
                {
                    key.SignData(serializedCert, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
                }
            }
        }

        [Fact]
        public static void CopyResult_RawData()
        {
            using (X509Certificate2 cert = new X509Certificate2(TestData.MsCertificate))
            {
                byte[] first = cert.RawData;
                byte[] second = cert.RawData;
                Assert.NotSame(first, second);
            }
        }

        [Fact]
        public static void RawDataMemory_NoCopy()
        {
            using (X509Certificate2 cert = new X509Certificate2(TestData.MsCertificate))
            {
                ReadOnlyMemory<byte> first = cert.RawDataMemory;
                ReadOnlyMemory<byte> second = cert.RawDataMemory;
                Assert.True(first.Span == second.Span, "RawDataMemory returned different values.");
            }
        }

        [Fact]
        public static void RawDataMemory_RoundTrip_LifetimeIndependentOfCert()
        {
            ReadOnlyMemory<byte> memory;

            using (X509Certificate2 cert = new X509Certificate2(TestData.MsCertificate))
            {
                memory = cert.RawDataMemory;
            }

            AssertExtensions.SequenceEqual(TestData.MsCertificate.AsSpan(), memory.Span);
        }

        [Fact]
        public static void MutateDistinguishedName_IssuerName_DoesNotImpactIssuer()
        {
            using (X509Certificate2 cert = new X509Certificate2(TestData.MsCertificate))
            {
                byte[] issuerBytes = cert.IssuerName.RawData;
                Array.Clear(issuerBytes);
                Assert.Equal("CN=Microsoft Code Signing PCA, O=Microsoft Corporation, L=Redmond, S=Washington, C=US", cert.Issuer);
            }
        }

        [Fact]
        public static void MutateDistinguishedName_SubjectName_DoesNotImpactSubject()
        {
            using (X509Certificate2 cert = new X509Certificate2(TestData.MsCertificate))
            {
                byte[] subjectBytes = cert.SubjectName.RawData;
                Array.Clear(subjectBytes);
                Assert.Equal("CN=Microsoft Corporation, OU=MOPR, O=Microsoft Corporation, L=Redmond, S=Washington, C=US", cert.Subject);
            }
        }

        [Fact]
        public static void SignatureAlgorithmOidReadableForGostCertificate()
        {
            using (X509Certificate2 cert = new X509Certificate2(TestData.GostCertificate))
            {
                Assert.Equal("1.2.643.2.2.3", cert.SignatureAlgorithm.Value);
            }
        }

        [Fact]
        public static void CertificateWithTrailingDataCanBeRead()
        {
            byte[] certData = new byte[TestData.MsCertificate.Length + 100];
            TestData.MsCertificate.AsSpan().CopyTo(certData);
            certData.AsSpan(TestData.MsCertificate.Length).Fill(0xFF);

            using (X509Certificate2 cert = new X509Certificate2(certData))
            {
                Assert.Equal("CN=Microsoft Corporation, OU=MOPR, O=Microsoft Corporation, L=Redmond, S=Washington, C=US", cert.Subject);
                Assert.Equal("CN=Microsoft Code Signing PCA, O=Microsoft Corporation, L=Redmond, S=Washington, C=US", cert.Issuer);

                Assert.Equal(TestData.MsCertificate, cert.RawData);
            }
        }

        [Fact]
        public static void CertificateSha3Signed()
        {
            using (X509Certificate2 cert = new X509Certificate2(TestData.RsaSha3_256SignedCertificate))
            {
                Assert.Equal("CN=potato", cert.Subject);

                using (RSA rsa = cert.PublicKey.GetRSAPublicKey())
                {
                    Assert.NotNull(rsa);
                }
            }
        }

        [ConditionalFact(typeof(PlatformSupport), nameof(PlatformSupport.PlatformCryptoProviderFunctionalP256))]
        [OuterLoop("Hardware backed key generation takes several seconds.", ~TestPlatforms.Browser)]
        public static void CreateCertificate_MicrosoftPlatformCryptoProvider_EcdsaKey()
        {
            using (CngKeyWrapper platformKey = CngKeyWrapper.CreateMicrosoftPlatformCryptoProvider(CngAlgorithm.ECDsaP256))
            using (ECDsaCng ecdsa = new ECDsaCng(platformKey.Key))
            {
                CertificateRequest req = new CertificateRequest("CN=potato", ecdsa, HashAlgorithmName.SHA256);

                using (X509Certificate2 cert = req.CreateSelfSigned(DateTimeOffset.UtcNow, DateTimeOffset.UtcNow))
                using (ECDsa certKey = cert.GetECDsaPrivateKey())
                {
                    Assert.NotNull(certKey);
                    byte[] data = new byte[] { 12, 11, 02, 08, 25, 14, 11, 18, 16 };
                    byte[] signature = certKey.SignData(data, HashAlgorithmName.SHA256);
                    bool valid = ecdsa.VerifyData(data, signature, HashAlgorithmName.SHA256);
                    Assert.True(valid, "valid signature");
                }
            }
        }

        [ConditionalFact(typeof(PlatformSupport), nameof(PlatformSupport.PlatformCryptoProviderFunctionalRsa))]
        [OuterLoop("Hardware backed key generation takes several seconds.", ~TestPlatforms.Browser)]
        public static void CreateCertificate_MicrosoftPlatformCryptoProvider_RsaKey()
        {
            using (CngKeyWrapper platformKey = CngKeyWrapper.CreateMicrosoftPlatformCryptoProvider(CngAlgorithm.Rsa))
            using (RSACng rsa = new RSACng(platformKey.Key))
            {
                CertificateRequest req = new CertificateRequest("CN=potato", rsa, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);

                using (X509Certificate2 cert = req.CreateSelfSigned(DateTimeOffset.UtcNow, DateTimeOffset.UtcNow))
                using (RSA certKey = cert.GetRSAPrivateKey())
                {
                    Assert.NotNull(certKey);
                    byte[] data = new byte[] { 12, 11, 02, 08, 25, 14, 11, 18, 16 };
                    byte[] signature = certKey.SignData(data, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
                    bool valid = rsa.VerifyData(data, signature, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
                    Assert.True(valid, "valid signature");
                }
            }
        }

        public static IEnumerable<object[]> StorageFlags => CollectionImportTests.StorageFlags;
    }
}
