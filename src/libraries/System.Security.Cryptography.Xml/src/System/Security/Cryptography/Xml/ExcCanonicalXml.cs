// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.IO;
using System.Text;
using System.Xml;

namespace System.Security.Cryptography.Xml
{
    internal sealed class ExcCanonicalXml
    {
        private readonly CanonicalXmlDocument _c14nDoc;
        private readonly ExcAncestralNamespaceContextManager _ancMgr;

        internal ExcCanonicalXml(Stream inputStream, bool includeComments, string inclusiveNamespacesPrefixList, XmlResolver resolver, string strBaseUri)
        {
            ArgumentNullException.ThrowIfNull(inputStream);

            _c14nDoc = new CanonicalXmlDocument(true, includeComments);
            _c14nDoc.XmlResolver = resolver;
            _c14nDoc.Load(Utils.PreProcessStreamInput(inputStream, resolver, strBaseUri));
            _ancMgr = new ExcAncestralNamespaceContextManager(inclusiveNamespacesPrefixList);
        }

        internal ExcCanonicalXml(XmlDocument document, bool includeComments, string inclusiveNamespacesPrefixList, XmlResolver resolver)
        {
            ArgumentNullException.ThrowIfNull(document);

            _c14nDoc = new CanonicalXmlDocument(true, includeComments);
            _c14nDoc.XmlResolver = resolver;
            _c14nDoc.Load(new XmlNodeReader(document));
            _ancMgr = new ExcAncestralNamespaceContextManager(inclusiveNamespacesPrefixList);
        }

        internal ExcCanonicalXml(XmlNodeList nodeList, bool includeComments, string inclusiveNamespacesPrefixList, XmlResolver resolver)
        {
            ArgumentNullException.ThrowIfNull(nodeList);

            XmlDocument? doc = Utils.GetOwnerDocument(nodeList);
            if (doc == null)
                throw new ArgumentException(nameof(nodeList));

            _c14nDoc = new CanonicalXmlDocument(false, includeComments);
            _c14nDoc.XmlResolver = resolver;
            _c14nDoc.Load(new XmlNodeReader(doc));
            _ancMgr = new ExcAncestralNamespaceContextManager(inclusiveNamespacesPrefixList);

            MarkInclusionStateForNodes(nodeList, doc, _c14nDoc);
        }

        internal byte[] GetBytes()
        {
            StringBuilder sb = new StringBuilder();
            _c14nDoc.Write(sb, DocPosition.BeforeRootElement, _ancMgr);
            return Encoding.UTF8.GetBytes(sb.ToString());
        }

        internal byte[] GetDigestedBytes(HashAlgorithm hash)
        {
            _c14nDoc.WriteHash(hash, DocPosition.BeforeRootElement, _ancMgr);
            hash.TransformFinalBlock(Array.Empty<byte>(), 0, 0);
            byte[] res = (byte[])hash.Hash!.Clone();
            // reinitialize the hash so it is still usable after the call
            hash.Initialize();
            return res;
        }

        private static void MarkInclusionStateForNodes(XmlNodeList nodeList, XmlDocument inputRoot, XmlDocument root)
        {
            CanonicalXmlNodeList elementList = new CanonicalXmlNodeList();
            CanonicalXmlNodeList elementListCanonical = new CanonicalXmlNodeList();
            elementList.Add(inputRoot);
            elementListCanonical.Add(root);
            int index = 0;

            do
            {
                XmlNode currentNode = (XmlNode)elementList[index]!;
                XmlNode currentNodeCanonical = (XmlNode)elementListCanonical[index]!;
                XmlNodeList childNodes = currentNode.ChildNodes;
                XmlNodeList childNodesCanonical = currentNodeCanonical.ChildNodes;
                for (int i = 0; i < childNodes.Count; i++)
                {
                    elementList.Add(childNodes[i]);
                    elementListCanonical.Add(childNodesCanonical[i]);

                    if (Utils.NodeInList(childNodes[i], nodeList))
                    {
                        MarkNodeAsIncluded(childNodesCanonical[i]!);
                    }

                    XmlAttributeCollection? attribNodes = childNodes[i]!.Attributes;
                    if (attribNodes != null)
                    {
                        for (int j = 0; j < attribNodes.Count; j++)
                        {
                            if (Utils.NodeInList(attribNodes[j], nodeList))
                            {
                                MarkNodeAsIncluded(childNodesCanonical[i]!.Attributes!.Item(j)!);
                            }
                        }
                    }
                }
                index++;
            } while (index < elementList.Count);
        }

        private static void MarkNodeAsIncluded(XmlNode node)
        {
            if (node is ICanonicalizableNode)
                ((ICanonicalizableNode)node).IsInNodeSet = true;
        }
    }
}
