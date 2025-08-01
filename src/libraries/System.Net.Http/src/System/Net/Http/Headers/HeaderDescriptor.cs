// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Buffers;
using System.Diagnostics;
using System.Diagnostics.CodeAnalysis;
using System.Text;
using System.Text.Unicode;

namespace System.Net.Http.Headers
{
    // This struct represents a particular named header --
    // if the header is one of our known headers, then it contains a reference to the KnownHeader object;
    // otherwise, for custom headers, it just contains a string for the header name.
    // Use HeaderDescriptor.TryGet to resolve an arbitrary header name to a HeaderDescriptor.
    internal readonly struct HeaderDescriptor : IEquatable<HeaderDescriptor>
    {
        private static readonly SearchValues<byte> s_dangerousCharacterBytes = SearchValues.Create((byte)'\0', (byte)'\r', (byte)'\n');

        /// <summary>
        /// Either a <see cref="KnownHeader"/> or <see cref="string"/>.
        /// </summary>
        private readonly object _descriptor;

        public HeaderDescriptor(KnownHeader knownHeader)
        {
            _descriptor = knownHeader;
        }

        // This should not be used directly; use static TryGet below
        internal HeaderDescriptor(string headerName, bool customHeader = false)
        {
            Debug.Assert(customHeader || KnownHeaders.TryGetKnownHeader(headerName) is null, $"The {nameof(KnownHeader)} overload should be used for {headerName}");
            _descriptor = headerName;
        }

        public string Name => _descriptor is KnownHeader header ? header.Name : (_descriptor as string)!;
        public HttpHeaderParser? Parser => (_descriptor as KnownHeader)?.Parser;
        public HttpHeaderType HeaderType => _descriptor is KnownHeader knownHeader ? knownHeader.HeaderType : HttpHeaderType.Custom;
        public KnownHeader? KnownHeader => _descriptor as KnownHeader;

        public bool Equals(KnownHeader other) => ReferenceEquals(_descriptor, other);

        public bool Equals(HeaderDescriptor other)
        {
            if (_descriptor is string headerName)
            {
                return string.Equals(headerName, other._descriptor as string, StringComparison.OrdinalIgnoreCase);
            }
            else
            {
                return ReferenceEquals(_descriptor, other._descriptor);
            }
        }

        public override int GetHashCode() => _descriptor is KnownHeader knownHeader ? knownHeader.GetHashCode() : StringComparer.OrdinalIgnoreCase.GetHashCode(_descriptor);

        public override bool Equals(object? obj) => throw new InvalidOperationException();   // Ensure this is never called, to avoid boxing

        // Returns false for invalid header name.
        public static bool TryGet(string headerName, out HeaderDescriptor descriptor)
        {
            Debug.Assert(!string.IsNullOrEmpty(headerName));

            KnownHeader? knownHeader = KnownHeaders.TryGetKnownHeader(headerName);
            if (knownHeader != null)
            {
                descriptor = new HeaderDescriptor(knownHeader);
                return true;
            }

            if (!HttpRuleParser.IsToken(headerName))
            {
                descriptor = default(HeaderDescriptor);
                return false;
            }

            descriptor = new HeaderDescriptor(headerName);
            return true;
        }

        // Returns false for invalid header name.
        public static bool TryGet(ReadOnlySpan<byte> headerName, out HeaderDescriptor descriptor)
        {
            Debug.Assert(headerName.Length > 0);

            KnownHeader? knownHeader = KnownHeaders.TryGetKnownHeader(headerName);
            if (knownHeader != null)
            {
                descriptor = new HeaderDescriptor(knownHeader);
                return true;
            }

            if (!HttpRuleParser.IsToken(headerName))
            {
                descriptor = default(HeaderDescriptor);
                return false;
            }

            descriptor = new HeaderDescriptor(HttpRuleParser.GetTokenString(headerName));
            return true;
        }

        internal static bool TryGetStaticQPackHeader(int index, out HeaderDescriptor descriptor, [NotNullWhen(true)] out string? knownValue)
        {
            Debug.Assert(index >= 0);

            // Micro-opt: store field to variable to prevent Length re-read and use unsigned to avoid bounds check.
            (HeaderDescriptor descriptor, string value)[] qpackStaticTable = QPackStaticTable.HeaderLookup;
            Debug.Assert(qpackStaticTable.Length == 99);

            uint uindex = (uint)index;

            if (uindex < (uint)qpackStaticTable.Length)
            {
                (descriptor, knownValue) = qpackStaticTable[uindex];
                return true;
            }
            else
            {
                descriptor = default;
                knownValue = null;
                return false;
            }
        }

        public HeaderDescriptor AsCustomHeader()
        {
            Debug.Assert(_descriptor is KnownHeader);
            Debug.Assert(HeaderType != HttpHeaderType.Custom);
            return new HeaderDescriptor(Name, customHeader: true);
        }

        public string GetHeaderValue(ReadOnlySpan<byte> headerValue, Encoding? valueEncoding)
        {
            if (headerValue.Length == 0)
            {
                return string.Empty;
            }

            // If it's a known header value, use the known value instead of allocating a new string.
            if (_descriptor is KnownHeader knownHeader)
            {
                if (knownHeader.KnownValues is string[] knownValues)
                {
                    for (int i = 0; i < knownValues.Length; i++)
                    {
                        if (Ascii.Equals(headerValue, knownValues[i]))
                        {
                            return knownValues[i];
                        }
                    }
                }

                if (knownHeader == KnownHeaders.ContentType)
                {
                    string? contentType = GetKnownContentType(headerValue);
                    if (contentType != null)
                    {
                        return contentType;
                    }
                }
                else if (knownHeader == KnownHeaders.Location)
                {
                    // Normally Location should be in ISO-8859-1 but occasionally some servers respond with UTF-8.
                    // If the user set the ResponseHeaderEncodingSelector, we give that priority instead.
                    if (valueEncoding is null && TryDecodeUtf8(headerValue, out string? decoded))
                    {
                        return decoded;
                    }
                }
            }

            string value = (valueEncoding ?? HttpRuleParser.DefaultHttpEncoding).GetString(headerValue);
            if (headerValue.ContainsAny(s_dangerousCharacterBytes))
            {
                // Depending on the encoding, 'value' may contain a dangerous character.
                // We are replacing them with SP to conform with https://www.rfc-editor.org/rfc/rfc9110.html#section-5.5-5.
                // This is a low-occurrence corner case, so we don't care about the cost of Replace() and the extra allocations.
                value = value.Replace('\0', ' ').Replace('\r', ' ').Replace('\n', ' ');
            }

            return value;
        }

        internal static string? GetKnownContentType(ReadOnlySpan<byte> contentTypeValue)
        {
            string? candidate = null;
            switch (contentTypeValue.Length)
            {
                case 8:
                    switch (contentTypeValue[7])
                    {
                        case (byte)'l': candidate = "text/xml"; break; // text/xm[l]
                        case (byte)'s': candidate = "text/css"; break; // text/cs[s]
                        case (byte)'v': candidate = "text/csv"; break; // text/cs[v]
                    }
                    break;

                case 9:
                    switch (contentTypeValue[6])
                    {
                        case (byte)'g': candidate = "image/gif"; break; // image/[g]if
                        case (byte)'p': candidate = "image/png"; break; // image/[p]ng
                        case (byte)'t': candidate = "text/html"; break; // text/h[t]ml
                    }
                    break;

                case 10:
                    switch (contentTypeValue[6])
                    {
                        case (byte)'l': candidate = "text/plain"; break; // text/p[l]ain
                        case (byte)'j': candidate = "image/jpeg"; break; // image/[j]peg
                        case (byte)'w': candidate = "image/webp"; break; // image/[w]ebp
                    }
                    break;

                case 13:
                    candidate = "image/svg+xml"; // image/svg+xml
                    break;

                case 15:
                    switch (contentTypeValue[12])
                    {
                        case (byte)'p': candidate = "application/pdf"; break; // application/[p]df
                        case (byte)'x': candidate = "application/xml"; break; // application/[x]ml
                        case (byte)'z': candidate = "application/zip"; break; // application/[z]ip
                        case (byte)'i': candidate = "text/javascript"; break; // text/javascr[i]pt
                    }
                    break;

                case 16:
                    switch (contentTypeValue[12])
                    {
                        case (byte)'g': candidate = "application/grpc"; break; // application/[g]rpc
                        case (byte)'j': candidate = "application/json"; break; // application/[j]son
                    }
                    break;

                case 17:
                    candidate = "text/event-stream"; // text/event-stream
                    break;

                case 19:
                    candidate = "multipart/form-data"; // multipart/form-data
                    break;

                case 22:
                    candidate = "application/javascript"; // application/javascript
                    break;

                case 23:
                    switch (contentTypeValue[18])
                    {
                        case (byte)'u': candidate = "text/html;charset=utf-8"; break; // text/html;charset=[u]tf-8
                        case (byte)'U': candidate = "text/html;charset=UTF-8"; break; // text/html;charset=[U]TF-8
                    }
                    break;

                case 24:
                    switch (contentTypeValue[10] ^ contentTypeValue[19])
                    {
                        case 'n' ^ 't': candidate = "application/octet-stream"; break; // applicatio[n]/octet-s[t]ream
                        case ' ' ^ 'u': candidate = "text/html; charset=utf-8"; break; // text/html;[ ]charset=[u]tf-8
                        case ' ' ^ 'U': candidate = "text/html; charset=UTF-8"; break; // text/html;[ ]charset=[U]TF-8
                        case ';' ^ 'u': candidate = "text/plain;charset=utf-8"; break; // text/plain[;]charset=[u]tf-8
                        case ';' ^ 'U': candidate = "text/plain;charset=UTF-8"; break; // text/plain[;]charset=[U]TF-8
                    }
                    break;

                case 25:
                    switch (contentTypeValue[20])
                    {
                        case (byte)'u': candidate = "text/plain; charset=utf-8"; break; // text/plain; charset=[u]tf-8
                        case (byte)'U': candidate = "text/plain; charset=UTF-8"; break; // text/plain; charset=[U]TF-8
                    }
                    break;

                case 29:
                    switch (contentTypeValue[19])
                    {
                        case (byte)'I': candidate = "text/html; charset=ISO-8859-1"; break; // text/html; charset=[I]SO-8859-1
                        case (byte)'i': candidate = "text/html; charset=iso-8859-1"; break; // text/html; charset=[i]so-8859-1
                    }
                    break;

                case 30:
                    switch (contentTypeValue[25])
                    {
                        case (byte)'u': candidate = "text/javascript; charset=utf-8"; break; // text/javascript; charset=[u]tf-8
                        case (byte)'U': candidate = "text/javascript; charset=UTF-8"; break; // text/javascript; charset=[U]TF-8
                    }
                    break;

                case 31:
                    candidate = "application/json; charset=utf-8"; // application/json; charset=utf-8
                    break;

                case 33:
                    candidate = "application/x-www-form-urlencoded"; // application/x-www-form-urlencoded
                    break;
            }

            Debug.Assert(candidate is null || candidate.Length == contentTypeValue.Length);

            return candidate != null && Ascii.Equals(contentTypeValue, candidate) ?
                candidate :
                null;
        }

        private static bool TryDecodeUtf8(ReadOnlySpan<byte> input, [NotNullWhen(true)] out string? decoded)
        {
            char[] rented = ArrayPool<char>.Shared.Rent(input.Length);

            try
            {
                if (Utf8.ToUtf16(input, rented, out _, out int charsWritten, replaceInvalidSequences: false) == OperationStatus.Done)
                {
                    decoded = new string(rented, 0, charsWritten);
                    return true;
                }
            }
            finally
            {
                ArrayPool<char>.Shared.Return(rented);
            }

            decoded = null;
            return false;
        }

        public string Separator => Parser is { } parser ? parser.Separator : HttpHeaderParser.DefaultSeparator;

        public byte[] SeparatorBytes => Parser is { } parser ? parser.SeparatorBytes : HttpHeaderParser.DefaultSeparatorBytes;
    }
}
