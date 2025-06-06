// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

//
// This is where we group together all the runtime export calls.
//

using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

using Internal.Runtime;

namespace System.Runtime
{
    internal static partial class RuntimeExports
    {
        //
        // internal calls for allocation
        //
        [RuntimeExport("RhNewObject")]
        public static unsafe object RhNewObject(MethodTable* pEEType)
        {
            // This is structured in a funny way because at the present state of things, the Debug.Assert
            // below will call into the assert defined in the class library (and not the MRT version of it). The one
            // in the class library is not low level enough to be callable when GC statics are not initialized yet.
            // Feel free to restructure once that's not a problem.
#if DEBUG
            bool isValid = !pEEType->IsGenericTypeDefinition &&
                !pEEType->IsInterface &&
                !pEEType->IsArray &&
                !pEEType->IsString &&
                !pEEType->IsPointer &&
                !pEEType->IsFunctionPointer &&
                !pEEType->IsByRefLike;
            if (!isValid)
                Debug.Assert(false);
#endif

#if FEATURE_64BIT_ALIGNMENT
            if (pEEType->RequiresAlign8)
            {
                if (pEEType->IsValueType)
                    return InternalCalls.RhpNewFastMisalign(pEEType);
                if (pEEType->IsFinalizable)
                    return InternalCalls.RhpNewFinalizableAlign8(pEEType);
                return InternalCalls.RhpNewFastAlign8(pEEType);
            }
            else
#endif // FEATURE_64BIT_ALIGNMENT
            {
                if (pEEType->IsFinalizable)
                    return InternalCalls.RhpNewFinalizable(pEEType);
                return InternalCalls.RhpNewFast(pEEType);
            }
        }

        [RuntimeExport("RhNewArray")]
        public static unsafe object RhNewArray(MethodTable* pEEType, int length)
        {
            Debug.Assert(pEEType->IsSzArray);

#if FEATURE_64BIT_ALIGNMENT
            MethodTable* pEEElementType = pEEType->RelatedParameterType;
            if (pEEElementType->IsValueType && pEEElementType->RequiresAlign8)
            {
                return InternalCalls.RhpNewArrayFastAlign8(pEEType, length);
            }
            else
#endif // FEATURE_64BIT_ALIGNMENT
            {
                return InternalCalls.RhpNewArrayFast(pEEType, length);
            }
        }

        [RuntimeExport("RhNewVariableSizeObject")]
        public static unsafe object RhNewVariableSizeObject(MethodTable* pEEType, int length)
        {
            Debug.Assert(pEEType->IsArray || pEEType->IsString);

            object array;
#if FEATURE_64BIT_ALIGNMENT
            MethodTable* pEEElementType = pEEType->RelatedParameterType;
            if (pEEElementType->IsValueType && pEEElementType->RequiresAlign8)
            {
                RuntimeImports.RhAllocateNewArray(pEEType, (uint)length, (uint)GC_ALLOC_FLAGS.GC_ALLOC_ALIGN8, &array);
            }
            else
#endif // FEATURE_64BIT_ALIGNMENT
            {
                RuntimeImports.RhAllocateNewArray(pEEType, (uint)length, (uint)GC_ALLOC_FLAGS.GC_ALLOC_NO_FLAGS, &array);
            }

            if (array == null)
                throw new OutOfMemoryException();

            return array;
        }

        public static unsafe object RhBox(MethodTable* pEEType, ref byte data)
        {
            // A null can be passed for boxing of a null ref.
            _ = Unsafe.ReadUnaligned<byte>(ref data);

            ref byte dataAdjustedForNullable = ref data;

            // Can box non-ByRefLike value types only (which also implies no finalizers).
            Debug.Assert(pEEType->IsValueType && !pEEType->IsByRefLike && !pEEType->IsFinalizable);

            // If we're boxing a Nullable<T> then either box the underlying T or return null (if the
            // nullable's value is empty).
            if (pEEType->IsNullable)
            {
                // The boolean which indicates whether the value is null comes first in the Nullable struct.
                if (data == 0)
                    return null;

                // Switch type we're going to box to the Nullable<T> target type and advance the data pointer
                // to the value embedded within the nullable.
                dataAdjustedForNullable = ref Unsafe.Add(ref data, pEEType->NullableValueOffset);
                pEEType = pEEType->NullableType;
            }

            object result;
#if FEATURE_64BIT_ALIGNMENT
            if (pEEType->RequiresAlign8)
            {
                result = InternalCalls.RhpNewFastMisalign(pEEType);
            }
            else
#endif // FEATURE_64BIT_ALIGNMENT
            {
                result = InternalCalls.RhpNewFast(pEEType);
            }

            // Copy the unboxed value type data into the new object.
            // Perform any write barriers necessary for embedded reference fields.
            if (pEEType->ContainsGCPointers)
            {
                InternalCalls.RhBulkMoveWithWriteBarrier(ref result.GetRawData(), ref dataAdjustedForNullable, pEEType->ValueTypeSize);
            }
            else
            {
                Unsafe.CopyBlock(ref result.GetRawData(), ref dataAdjustedForNullable, pEEType->ValueTypeSize);
            }

            return result;
        }

        [RuntimeExport("RhBoxAny")]
        public static unsafe object RhBoxAny(ref byte data, MethodTable* pEEType)
        {
            if (pEEType->IsValueType)
            {
                return RhBox(pEEType, ref data);
            }
            else
            {
                return Unsafe.As<byte, object>(ref data);
            }
        }

        private static unsafe bool UnboxAnyTypeCompare(MethodTable* pEEType, MethodTable* ptrUnboxToEEType)
        {
            if (pEEType == ptrUnboxToEEType)
                return true;

            if (pEEType->ElementType == ptrUnboxToEEType->ElementType)
            {
                // Enum's and primitive types should pass the UnboxAny exception cases
                // if they have an exactly matching cor element type.
                switch (ptrUnboxToEEType->ElementType)
                {
                    case EETypeElementType.Byte:
                    case EETypeElementType.SByte:
                    case EETypeElementType.Int16:
                    case EETypeElementType.UInt16:
                    case EETypeElementType.Int32:
                    case EETypeElementType.UInt32:
                    case EETypeElementType.Int64:
                    case EETypeElementType.UInt64:
                    case EETypeElementType.IntPtr:
                    case EETypeElementType.UIntPtr:
                        return true;
                }
            }

            return false;
        }

        [RuntimeExport("RhUnboxAny")]
        public static unsafe void RhUnboxAny(object? o, ref byte data, MethodTable* pUnboxToEEType)
        {
            if (pUnboxToEEType->IsValueType)
            {
                bool isValid = false;

                if (pUnboxToEEType->IsNullable)
                {
                    isValid = (o == null) || o.GetMethodTable() == pUnboxToEEType->NullableType;
                }
                else
                {
                    isValid = (o != null) && UnboxAnyTypeCompare(o.GetMethodTable(), pUnboxToEEType);
                }

                if (!isValid)
                {
                    // Throw the invalid cast exception defined by the classlib, using the input unbox MethodTable*
                    // to find the correct classlib.

                    ExceptionIDs exID = o == null ? ExceptionIDs.NullReference : ExceptionIDs.InvalidCast;

                    throw pUnboxToEEType->GetClasslibException(exID);
                }

                RhUnbox(o, ref data, pUnboxToEEType);
            }
            else
            {
                if (o != null && (TypeCast.IsInstanceOfAny(pUnboxToEEType, o) == null))
                {
                    throw pUnboxToEEType->GetClasslibException(ExceptionIDs.InvalidCast);
                }

                Unsafe.As<byte, object?>(ref data) = o;
            }
        }

        //
        // Unbox helpers with RyuJIT conventions
        //
        public static unsafe ref byte RhUnbox2(MethodTable* pUnboxToEEType, object obj)
        {
            if ((obj == null) || !UnboxAnyTypeCompare(obj.GetMethodTable(), pUnboxToEEType))
            {
                ExceptionIDs exID = obj == null ? ExceptionIDs.NullReference : ExceptionIDs.InvalidCast;
                throw pUnboxToEEType->GetClasslibException(exID);
            }
            return ref obj.GetRawData();
        }

        public static unsafe void RhUnboxNullable(ref byte data, MethodTable* pUnboxToEEType, object obj)
        {
            if (obj != null && obj.GetMethodTable() != pUnboxToEEType->NullableType)
            {
                throw pUnboxToEEType->GetClasslibException(ExceptionIDs.InvalidCast);
            }
            RhUnbox(obj, ref data, pUnboxToEEType);
        }

        public static unsafe void RhUnboxTypeTest(MethodTable* pType, MethodTable* pBoxType)
        {
            Debug.Assert(pType->IsValueType);

            if (!UnboxAnyTypeCompare(pType, pBoxType))
            {
                throw pType->GetClasslibException(ExceptionIDs.InvalidCast);
            }
        }

        [RuntimeExport("RhUnbox")]
        public static unsafe void RhUnbox(object? obj, ref byte data, MethodTable* pUnboxToEEType)
        {
            // When unboxing to a Nullable the input object may be null.
            if (obj == null)
            {
                Debug.Assert(pUnboxToEEType != null && pUnboxToEEType->IsNullable);

                // Set HasValue to false and clear the value (in case there were GC references we wish to stop reporting).
                InternalCalls.RhpGcSafeZeroMemory(
                    ref data,
                    pUnboxToEEType->ValueTypeSize);

                return;
            }

            MethodTable* pEEType = obj.GetMethodTable();

            // Can unbox value types only.
            Debug.Assert(pEEType->IsValueType);

            // A special case is that we can unbox a value type T into a Nullable<T>. It's the only case where
            // pUnboxToEEType is useful.
            Debug.Assert((pUnboxToEEType == null) || UnboxAnyTypeCompare(pEEType, pUnboxToEEType) || pUnboxToEEType->IsNullable);
            if (pUnboxToEEType != null && pUnboxToEEType->IsNullable)
            {
                Debug.Assert(pUnboxToEEType->NullableType == pEEType);

                // Set the first field of the Nullable to true to indicate the value is present.
                Unsafe.As<byte, bool>(ref data) = true;

                // Adjust the data pointer so that it points at the value field in the Nullable.
                data = ref Unsafe.Add(ref data, pUnboxToEEType->NullableValueOffset);
            }

            ref byte fields = ref obj.GetRawData();

            if (pEEType->ContainsGCPointers)
            {
                // Copy the boxed fields into the new location in a GC safe manner
                InternalCalls.RhBulkMoveWithWriteBarrier(ref data, ref fields, pEEType->ValueTypeSize);
            }
            else
            {
                // Copy the boxed fields into the new location.
                Unsafe.CopyBlock(ref data, ref fields, pEEType->ValueTypeSize);
            }
        }

        [RuntimeExport("RhGetCurrentThreadStackTrace")]
        [MethodImpl(MethodImplOptions.NoInlining)] // Ensures that the RhGetCurrentThreadStackTrace frame is always present
        public static unsafe int RhGetCurrentThreadStackTrace(IntPtr[] outputBuffer)
        {
            fixed (IntPtr* pOutputBuffer = outputBuffer)
                return RhpGetCurrentThreadStackTrace(pOutputBuffer, (uint)((outputBuffer != null) ? outputBuffer.Length : 0), new UIntPtr(&pOutputBuffer));
        }

#pragma warning disable SYSLIB1054 // Use DllImport here instead of LibraryImport because this file is used by Test.CoreLib.
        [DllImport("*")]
        private static extern unsafe int RhpGetCurrentThreadStackTrace(IntPtr* pOutputBuffer, uint outputBufferLength, UIntPtr addressInCurrentFrame);
#pragma warning restore SYSLIB1054

        // Worker for RhGetCurrentThreadStackTrace.  RhGetCurrentThreadStackTrace just allocates a transition
        // frame that will be used to seed the stack trace and this method does all the real work.
        //
        // Input:           outputBuffer may be null or non-null
        // Return value:    positive: number of entries written to outputBuffer
        //                  negative: number of required entries in outputBuffer in case it's too small (or null)
        // Output:          outputBuffer is filled in with return address IPs, starting with placing the this
        //                  method's return address into index 0
        //
        // NOTE: We don't want to allocate the array on behalf of the caller because we don't know which class
        // library's objects the caller understands (we support multiple class libraries with multiple root
        // System.Object types).
        [UnmanagedCallersOnly(EntryPoint = "RhpCalculateStackTraceWorker")]
        private static unsafe int RhpCalculateStackTraceWorker(IntPtr* pOutputBuffer, uint outputBufferLength, UIntPtr addressInCurrentFrame)
        {
            uint nFrames = 0;
            bool success = true;

            StackFrameIterator frameIter = default;

            bool isValid = frameIter.Init(null);
            Debug.Assert(isValid, "Missing RhGetCurrentThreadStackTrace frame");

            // Note that the while loop will skip RhGetCurrentThreadStackTrace frame
            while (frameIter.Next())
            {
                if ((void*)frameIter.SP < (void*)addressInCurrentFrame)
                    continue;

                if (nFrames < outputBufferLength)
                    pOutputBuffer[nFrames] = new IntPtr(frameIter.ControlPC);
                else
                    success = false;

                nFrames++;
            }

            return success ? (int)nFrames : -(int)nFrames;
        }

        [RuntimeExport("RhGetRuntimeHelperForType")]
        internal static unsafe IntPtr RhGetRuntimeHelperForType(MethodTable* pEEType, RuntimeHelperKind kind)
        {
            switch (kind)
            {
                case RuntimeHelperKind.AllocateObject:
#if FEATURE_64BIT_ALIGNMENT
                    if (pEEType->RequiresAlign8)
                    {
                        if (pEEType->IsFinalizable)
                            return (IntPtr)(delegate*<MethodTable*, object>)&InternalCalls.RhpNewFinalizableAlign8;
                        else if (pEEType->IsValueType)            // returns true for enum types as well
                            return (IntPtr)(delegate*<MethodTable*, object>)&InternalCalls.RhpNewFastMisalign;
                        else
                            return (IntPtr)(delegate*<MethodTable*, object>)&InternalCalls.RhpNewFastAlign8;
                    }
#endif // FEATURE_64BIT_ALIGNMENT

                    if (pEEType->IsFinalizable)
                        return (IntPtr)(delegate*<MethodTable*, object>)&InternalCalls.RhpNewFinalizable;
                    else
                        return (IntPtr)(delegate*<MethodTable*, object>)&InternalCalls.RhpNewFast;

                case RuntimeHelperKind.IsInst:
                    if (pEEType->HasGenericVariance || pEEType->IsParameterizedType || pEEType->IsFunctionPointer)
                        return (IntPtr)(delegate*<MethodTable*, object, object?>)&TypeCast.IsInstanceOfAny;
                    else if (pEEType->IsInterface)
                        return (IntPtr)(delegate*<MethodTable*, object?, object?>)&TypeCast.IsInstanceOfInterface;
                    else
                        return (IntPtr)(delegate*<MethodTable*, object?, object?>)&TypeCast.IsInstanceOfClass;

                case RuntimeHelperKind.CastClass:
                    if (pEEType->HasGenericVariance || pEEType->IsParameterizedType || pEEType->IsFunctionPointer)
                        return (IntPtr)(delegate*<MethodTable*, object, object>)&TypeCast.CheckCastAny;
                    else if (pEEType->IsInterface)
                        return (IntPtr)(delegate*<MethodTable*, object, object>)&TypeCast.CheckCastInterface;
                    else
                        return (IntPtr)(delegate*<MethodTable*, object, object>)&TypeCast.CheckCastClass;

                case RuntimeHelperKind.AllocateArray:
#if FEATURE_64BIT_ALIGNMENT
                    MethodTable* pEEElementType = pEEType->RelatedParameterType;
                    if (pEEElementType->IsValueType && pEEElementType->RequiresAlign8)
                        return (IntPtr)(delegate*<MethodTable*, int, object>)&InternalCalls.RhpNewArrayFastAlign8;
#endif // FEATURE_64BIT_ALIGNMENT

                    return (IntPtr)(delegate*<MethodTable*, int, object>)&InternalCalls.RhpNewArrayFast;

                default:
                    Debug.Fail("Unknown RuntimeHelperKind");
                    return IntPtr.Zero;
            }
        }
    }
}
