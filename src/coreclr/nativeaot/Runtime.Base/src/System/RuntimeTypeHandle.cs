// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// System.Type and System.RuntimeTypeHandle are defined here as the C# compiler requires them

using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace System
{
    public class Type
    {
        public RuntimeTypeHandle TypeHandle { get { return default(RuntimeTypeHandle); } }
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct RuntimeTypeHandle
    {
        private IntPtr _value;
    }
}
