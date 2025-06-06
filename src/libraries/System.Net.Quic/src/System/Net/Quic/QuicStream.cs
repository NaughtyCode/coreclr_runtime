// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Diagnostics;
using System.IO;
using System.Runtime.CompilerServices;
using System.Runtime.ExceptionServices;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.Quic;
using static System.Net.Quic.MsQuicHelpers;
using static Microsoft.Quic.MsQuic;
using PEER_RECEIVE_ABORTED_DATA = Microsoft.Quic.QUIC_STREAM_EVENT._Anonymous_e__Union._PEER_RECEIVE_ABORTED_e__Struct;
using PEER_SEND_ABORTED_DATA = Microsoft.Quic.QUIC_STREAM_EVENT._Anonymous_e__Union._PEER_SEND_ABORTED_e__Struct;
using RECEIVE_DATA = Microsoft.Quic.QUIC_STREAM_EVENT._Anonymous_e__Union._RECEIVE_e__Struct;
using SEND_COMPLETE_DATA = Microsoft.Quic.QUIC_STREAM_EVENT._Anonymous_e__Union._SEND_COMPLETE_e__Struct;
using SEND_SHUTDOWN_COMPLETE_DATA = Microsoft.Quic.QUIC_STREAM_EVENT._Anonymous_e__Union._SEND_SHUTDOWN_COMPLETE_e__Struct;
using SHUTDOWN_COMPLETE_DATA = Microsoft.Quic.QUIC_STREAM_EVENT._Anonymous_e__Union._SHUTDOWN_COMPLETE_e__Struct;
using START_COMPLETE_DATA = Microsoft.Quic.QUIC_STREAM_EVENT._Anonymous_e__Union._START_COMPLETE_e__Struct;

namespace System.Net.Quic;

/// <summary>
/// Represents a QUIC stream, see <see href="https://www.rfc-editor.org/rfc/rfc9000.html#name-streams">RFC 9000: Streams</see> for more details.
/// <see cref="QuicStream" /> can be <see cref="QuicStreamType.Unidirectional">unidirectional</see>, i.e.: write-only for the opening side,
/// or <see cref="QuicStreamType.Bidirectional">bidirectional</see> which allows both side to write.
/// </summary>
/// <remarks>
/// <see cref="QuicStream"/> can be used in a same way as any other <see cref="Stream"/>.
/// Apart from stream API, <see cref="QuicStream"/> also exposes QUIC specific features:
/// <list type="bullet">
/// <item>
/// <term><see cref="WriteAsync(System.ReadOnlyMemory{byte},bool,System.Threading.CancellationToken)"/></term>
/// <description>Allows to close the writing side of the stream as a single operation with the write itself.</description>
/// </item>
/// <item>
/// <term><see cref="CompleteWrites"/></term>
/// <description>Close the writing side of the stream.</description>
/// </item>
/// <item>
/// <term><see cref="Abort"/></term>
/// <description>Aborts either the writing or the reading side of the stream.</description>
/// </item>
/// <item>
/// <term><see cref="WritesClosed"/></term>
/// <description>A <see cref="Task"/> that will get completed when the stream writing side has been closed (gracefully or abortively).</description>
/// </item>
/// <item>
/// <term><see cref="ReadsClosed"/></term>
/// <description>A <see cref="Task"/> that will get completed when the stream reading side has been closed (gracefully or abortively).</description>
/// </item>
/// </list>
/// </remarks>
public sealed partial class QuicStream
{
    /// <summary>
    /// Handle to MsQuic connection object.
    /// </summary>
    private readonly MsQuicContextSafeHandle _handle;

    /// <summary>
    /// Set to true once disposed. Prevents double and/or concurrent disposal.
    /// </summary>
    private bool _disposed;

    private readonly ValueTaskSource _startedTcs = new ValueTaskSource();
    private readonly ValueTaskSource _shutdownTcs = new ValueTaskSource();

    private readonly ResettableValueTaskSource _receiveTcs = new ResettableValueTaskSource()
    {
        CancellationAction = target =>
        {
            try
            {
                if (target is QuicStream stream)
                {
                    stream.Abort(QuicAbortDirection.Read, stream._defaultErrorCode);
                    stream._receiveTcs.TrySetResult();
                }
            }
            catch (ObjectDisposedException)
            {
                // We collided with a Dispose in another thread. This can happen
                // when using CancellationTokenSource.CancelAfter.
                // Ignore the exception
            }
        }
    };
    private ReceiveBuffers _receiveBuffers = new ReceiveBuffers();
    private int _receivedNeedsEnable;

    private readonly ResettableValueTaskSource _sendTcs = new ResettableValueTaskSource()
    {
        CancellationAction = target =>
        {
            try
            {
                if (target is QuicStream stream)
                {
                    stream.Abort(QuicAbortDirection.Write, stream._defaultErrorCode);
                }
            }
            catch (ObjectDisposedException)
            {
                // We collided with a Dispose in another thread. This can happen
                // when using CancellationTokenSource.CancelAfter.
                // Ignore the exception
            }
        }
    };
    private MsQuicBuffers _sendBuffers = new MsQuicBuffers();
    private int _sendLocked;
    private Exception? _sendException;

    private readonly long _defaultErrorCode;

    private readonly bool _canRead;
    private readonly bool _canWrite;

    private long _id = -1;
    private readonly QuicStreamType _type;

    /// <summary>
    /// Provided via <see cref="StartAsync(Action{QuicStreamType}, CancellationToken)" /> from <see cref="QuicConnection" /> so that <see cref="QuicStream"/> can decrement its available stream count field.
    /// When <see cref="HandleEventStartComplete(ref START_COMPLETE_DATA)">START_COMPLETE</see> arrives it gets invoked and unset back to <c>null</c> to not to hold any unintended reference to <see cref="QuicConnection"/>.
    /// </summary>
    private Action<QuicStreamType>? _decrementStreamCapacity;

    /// <summary>
    /// Stream id, see <see href="https://www.rfc-editor.org/rfc/rfc9000.html#name-stream-types-and-identifier" />.
    /// </summary>
    public long Id => _id;

    /// <summary>
    /// Stream type, see <see href="https://www.rfc-editor.org/rfc/rfc9000.html#name-stream-types-and-identifier" />.
    /// </summary>
    public QuicStreamType Type => _type;

    /// <summary>
    /// A <see cref="Task"/> that will get completed once reading side has been closed.
    /// Which might be by reading till end of stream (<see cref="ReadAsync(System.Memory{byte},System.Threading.CancellationToken)"/> will return <c>0</c>),
    /// or when <see cref="Abort"/> for <see cref="QuicAbortDirection.Read"/> is called,
    /// or when the peer called <see cref="Abort"/> for <see cref="QuicAbortDirection.Write"/>.
    /// </summary>
    public Task ReadsClosed => _receiveTcs.GetFinalTask(this);

    /// <summary>
    /// A <see cref="Task"/> that will get completed once writing side has been closed.
    /// Which might be by closing the write side via <see cref="CompleteWrites"/>
    /// or <see cref="WriteAsync(System.ReadOnlyMemory{byte},bool,System.Threading.CancellationToken)"/> with <c>completeWrites: true</c> and getting acknowledgement from the peer for it,
    /// or when <see cref="Abort"/> for <see cref="QuicAbortDirection.Write"/> is called,
    /// or when the peer called <see cref="Abort"/> for <see cref="QuicAbortDirection.Read"/>.
    /// </summary>
    public Task WritesClosed => _sendTcs.GetFinalTask(this);

    /// <inheritdoc />
    public override string ToString() => _handle.ToString();

    /// <summary>
    /// Initializes a new instance of an outbound <see cref="QuicStream" />.
    /// </summary>
    /// <param name="connectionHandle"><see cref="QuicConnection"/> safe handle, used to increment/decrement reference count with each associated stream.</param>
    /// <param name="type">The type of the stream to open.</param>
    /// <param name="defaultErrorCode">Error code used when the stream needs to abort read or write side of the stream internally.</param>
    internal unsafe QuicStream(MsQuicContextSafeHandle connectionHandle, QuicStreamType type, long defaultErrorCode)
    {
        GCHandle context = GCHandle.Alloc(this, GCHandleType.Weak);
        try
        {
            QUIC_HANDLE* handle;
            ThrowHelper.ThrowIfMsQuicError(MsQuicApi.Api.StreamOpen(
                connectionHandle,
                type == QuicStreamType.Unidirectional ? QUIC_STREAM_OPEN_FLAGS.UNIDIRECTIONAL : QUIC_STREAM_OPEN_FLAGS.NONE,
                &NativeCallback,
                (void*)GCHandle.ToIntPtr(context),
                &handle),
                "StreamOpen failed");
            _handle = new MsQuicContextSafeHandle(handle, context, SafeHandleType.Stream, connectionHandle)
            {
                Disposable = _sendBuffers
            };
        }
        catch
        {
            context.Free();
            throw;
        }

        _defaultErrorCode = defaultErrorCode;

        _canRead = type == QuicStreamType.Bidirectional;
        _canWrite = true;
        if (!_canRead)
        {
            _receiveTcs.TrySetResult(final: true);
        }
        _type = type;
    }

    /// <summary>
    /// Initializes a new instance of an inbound <see cref="QuicStream" />.
    /// </summary>
    /// <param name="connectionHandle"><see cref="QuicConnection"/> safe handle, used to increment/decrement reference count with each associated stream.</param>
    /// <param name="handle">Native handle.</param>
    /// <param name="flags">Related data from the PEER_STREAM_STARTED connection event.</param>
    /// <param name="defaultErrorCode">Error code used when the stream needs to abort read or write side of the stream internally.</param>
    internal unsafe QuicStream(MsQuicContextSafeHandle connectionHandle, QUIC_HANDLE* handle, QUIC_STREAM_OPEN_FLAGS flags, long defaultErrorCode)
    {
        GCHandle context = GCHandle.Alloc(this, GCHandleType.Weak);
        try
        {
            _handle = new MsQuicContextSafeHandle(handle, context, SafeHandleType.Stream, connectionHandle)
            {
                Disposable = _sendBuffers
            };
            delegate* unmanaged[Cdecl]<QUIC_HANDLE*, void*, QUIC_STREAM_EVENT*, int> nativeCallback = &NativeCallback;
            MsQuicApi.Api.SetCallbackHandler(
                _handle,
                nativeCallback,
                (void*)GCHandle.ToIntPtr(context));
        }
        catch
        {
            context.Free();
            throw;
        }

        _defaultErrorCode = defaultErrorCode;

        _canRead = true;
        _canWrite = !flags.HasFlag(QUIC_STREAM_OPEN_FLAGS.UNIDIRECTIONAL);
        if (!_canWrite)
        {
            _sendTcs.TrySetResult(final: true);
        }
        _id = (long)GetMsQuicParameter<ulong>(_handle, QUIC_PARAM_STREAM_ID);
        _type = flags.HasFlag(QUIC_STREAM_OPEN_FLAGS.UNIDIRECTIONAL) ? QuicStreamType.Unidirectional : QuicStreamType.Bidirectional;

        _startedTcs.TrySetResult();
    }

    /// <summary>
    /// Starts the stream, but doesn't send anything to the peer yet.
    /// If no more concurrent streams can be opened at the moment, the operation will wait until it can,
    /// either by closing some existing streams or receiving more available stream ids from the peer.
    /// </summary>
    /// <param name="decrementStreamCapacity"></param>
    /// <param name="cancellationToken">A cancellation token that can be used to cancel the asynchronous operation.</param>
    /// <returns>An asynchronous task that completes with the opened <see cref="QuicStream" />.</returns>
    internal ValueTask StartAsync(Action<QuicStreamType> decrementStreamCapacity, CancellationToken cancellationToken = default)
    {
        Debug.Assert(!_startedTcs.IsCompleted);

        // Always call StreamStart to get consistent behavior (events, stream count, frames send to peer) regardless of cancellation.
        _startedTcs.TryInitialize(out ValueTask valueTask, this, cancellationToken);
        _decrementStreamCapacity = decrementStreamCapacity;
        unsafe
        {
            int status = MsQuicApi.Api.StreamStart(
                _handle,
                QUIC_STREAM_START_FLAGS.SHUTDOWN_ON_FAIL | QUIC_STREAM_START_FLAGS.INDICATE_PEER_ACCEPT);
            if (StatusFailed(status))
            {
                _decrementStreamCapacity = null;
                _startedTcs.TrySetException(ThrowHelper.GetExceptionForMsQuicStatus(status));
            }
        }
        return valueTask;
    }

    /// <inheritdoc />
    public override async ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);

        if (!_canRead)
        {
            throw new InvalidOperationException(SR.net_quic_reading_notallowed);
        }

        if (NetEventSource.Log.IsEnabled())
        {
            NetEventSource.Info(this, $"{this} Stream reading into memory of '{buffer.Length}' bytes.");
        }

        if (_receiveTcs.IsCompleted)
        {
            // Special case exception type for pre-canceled token while we've already transitioned to a final state and don't need to abort read.
            // It must happen before we try to get the value task, since the task source is versioned and each instance must be awaited.
            cancellationToken.ThrowIfCancellationRequested();
        }

        // The following loop will repeat at most twice depending whether some data are readily available in the buffer (one iteration) or not.
        // In which case, it'll wait on RECEIVE or any of PEER_SEND_(SHUTDOWN|ABORTED) event and attempt to copy data in the second iteration.
        int totalCopied = 0;
        do
        {
            // Concurrent call, this one lost the race.
            if (!_receiveTcs.TryGetValueTask(out ValueTask valueTask, this, cancellationToken))
            {
                throw new InvalidOperationException(SR.Format(SR.net_io_invalidnestedcall, "read"));
            }

            // Copy data from the buffer, reduce target and increment total.
            int copied = _receiveBuffers.CopyTo(buffer, out bool complete, out bool empty);
            buffer = buffer.Slice(copied);
            totalCopied += copied;

            // Make sure the task transitions into final state before the method finishes.
            if (complete)
            {
                _receiveTcs.TrySetResult(final: true);
            }

            // Unblock the next await to end immediately, i.e. there were/are any data in the buffer.
            if (totalCopied > 0 || !empty)
            {
                _receiveTcs.TrySetResult();
            }

            // This will either wait for RECEIVE event (no data in buffer) or complete immediately and reset the task.
            await valueTask.ConfigureAwait(false);

            // This is the last read, finish even despite not copying anything.
            if (complete)
            {
                break;
            }
        } while (!buffer.IsEmpty && totalCopied == 0);  // Exit the loop if target buffer is full we at least copied something.

        if (totalCopied > 0 && Interlocked.CompareExchange(ref _receivedNeedsEnable, 0, 1) == 1)
        {
            unsafe
            {
                ThrowHelper.ThrowIfMsQuicError(MsQuicApi.Api.StreamReceiveSetEnabled(
                    _handle,
                    1),
                "StreamReceivedSetEnabled failed");
            }
        }

        if (NetEventSource.Log.IsEnabled())
        {
            NetEventSource.Info(this, $"{this} Stream read '{totalCopied}' bytes.");
        }

        return totalCopied;
    }

    /// <inheritdoc />
    public override ValueTask WriteAsync(ReadOnlyMemory<byte> buffer, CancellationToken cancellationToken = default)
        => WriteAsync(buffer, completeWrites: false, cancellationToken);


    /// <inheritdoc cref="WriteAsync(ReadOnlyMemory{byte}, CancellationToken)"/>
    /// <param name="buffer">The region of memory to write data from.</param>
    /// <param name="cancellationToken">The token to monitor for cancellation requests. The default value is <see cref="CancellationToken.None"/>.</param>
    /// <param name="completeWrites">Notifies the peer about gracefully closing the write side, i.e.: sends FIN flag with the data.</param>
    public ValueTask WriteAsync(ReadOnlyMemory<byte> buffer, bool completeWrites, CancellationToken cancellationToken = default)
    {
        if (_disposed)
        {
            return ValueTask.FromException(ExceptionDispatchInfo.SetCurrentStackTrace(new ObjectDisposedException(nameof(QuicStream))));
        }

        if (!_canWrite)
        {
            return ValueTask.FromException(ExceptionDispatchInfo.SetCurrentStackTrace(new InvalidOperationException(SR.net_quic_writing_notallowed)));
        }

        if (NetEventSource.Log.IsEnabled())
        {
            NetEventSource.Info(this, $"{this} Stream writing memory of '{buffer.Length}' bytes while {(completeWrites ? "completing" : "not completing")} writes.");
        }

        if (_sendTcs.IsCompleted && cancellationToken.IsCancellationRequested)
        {
            // Special case exception type for pre-canceled token while we've already transitioned to a final state and don't need to abort write.
            // It must happen before we try to get the value task, since the task source is versioned and each instance must be awaited.
            return ValueTask.FromCanceled(cancellationToken);
        }

        // Concurrent call, this one lost the race.
        if (!_sendTcs.TryGetValueTask(out ValueTask valueTask, this, cancellationToken))
        {
            return ValueTask.FromException(ExceptionDispatchInfo.SetCurrentStackTrace(new InvalidOperationException(SR.Format(SR.net_io_invalidnestedcall, "write"))));
        }

        // No need to call anything since we already have a result, most likely an exception.
        if (valueTask.IsCompleted)
        {
            return valueTask;
        }

        // For an empty buffer complete immediately, close the writing side of the stream if necessary.
        if (buffer.IsEmpty)
        {
            _sendTcs.TrySetResult();
            if (completeWrites)
            {
                CompleteWrites();
            }
            return valueTask;
        }

        // We own the lock, abort might happen, but exception will get stored instead.
        if (Interlocked.CompareExchange(ref _sendLocked, 1, 0) == 0)
        {
            unsafe
            {
                _sendBuffers.Initialize(buffer);
                int status = MsQuicApi.Api.StreamSend(
                    _handle,
                    _sendBuffers.Buffers,
                    (uint)_sendBuffers.Count,
                    completeWrites ? QUIC_SEND_FLAGS.FIN : QUIC_SEND_FLAGS.NONE,
                    null);
                // No SEND_COMPLETE expected, release buffer and unlock.
                if (StatusFailed(status))
                {
                    _sendBuffers.Reset();
                    Volatile.Write(ref _sendLocked, 0);

                    // There might be stored exception from when we held the lock.
                    if (ThrowHelper.TryGetStreamExceptionForMsQuicStatus(status, out Exception? exception))
                    {
                        Interlocked.CompareExchange(ref _sendException, exception, null);
                    }
                    exception = Volatile.Read(ref _sendException);
                    if (exception is not null)
                    {
                        _sendTcs.TrySetException(exception);
                    }
                }
                // SEND_COMPLETE expected, buffer and lock will be released then.
            }
        }

        return valueTask;
    }

    /// <summary>
    /// Aborts either <see cref="QuicAbortDirection.Read">reading</see>, <see cref="QuicAbortDirection.Write">writing</see> or <see cref="QuicAbortDirection.Both">both</see> sides of the stream.
    /// </summary>
    /// <remarks>
    /// Corresponds to <see href="https://www.rfc-editor.org/rfc/rfc9000.html#frame-stop-sending">STOP_SENDING</see>
    /// and <see href="https://www.rfc-editor.org/rfc/rfc9000.html#frame-reset-stream">RESET_STREAM</see> QUIC frames.
    /// </remarks>
    /// <param name="abortDirection">The direction of the stream to abort.</param>
    /// <param name="errorCode">The error code with which to abort the stream, this value is application protocol (layer above QUIC) dependent.</param>
    public void Abort(QuicAbortDirection abortDirection, long errorCode)
    {
        if (_disposed)
        {
            return;
        }
        ThrowHelper.ValidateErrorCode(nameof(errorCode), errorCode, $"{nameof(Abort)}.{nameof(errorCode)}");

        QUIC_STREAM_SHUTDOWN_FLAGS flags = QUIC_STREAM_SHUTDOWN_FLAGS.NONE;
        if (abortDirection.HasFlag(QuicAbortDirection.Read) && !_receiveTcs.IsCompleted)
        {
            flags |= QUIC_STREAM_SHUTDOWN_FLAGS.ABORT_RECEIVE;
        }
        if (abortDirection.HasFlag(QuicAbortDirection.Write) && !_sendTcs.IsCompleted)
        {
            flags |= QUIC_STREAM_SHUTDOWN_FLAGS.ABORT_SEND;
        }
        // Nothing to abort, the requested sides to abort are already closed.
        if (flags == QUIC_STREAM_SHUTDOWN_FLAGS.NONE)
        {
            return;
        }

        if (NetEventSource.Log.IsEnabled())
        {
            NetEventSource.Info(this, $"{this} Aborting {abortDirection} with {errorCode}");
        }
        unsafe
        {
            ThrowHelper.ThrowIfMsQuicError(MsQuicApi.Api.StreamShutdown(
                _handle,
                flags,
                (ulong)errorCode),
                "StreamShutdown failed");
        }

        if (abortDirection.HasFlag(QuicAbortDirection.Read))
        {
            _receiveTcs.TrySetException(ThrowHelper.GetOperationAbortedException(SR.net_quic_reading_aborted));
        }
        if (abortDirection.HasFlag(QuicAbortDirection.Write))
        {
            var exception = ThrowHelper.GetOperationAbortedException(SR.net_quic_writing_aborted);
            Interlocked.CompareExchange(ref _sendException, exception, null);
            if (Interlocked.CompareExchange(ref _sendLocked, 1, 0) == 0)
            {
                _sendTcs.TrySetException(_sendException);
                Volatile.Write(ref _sendLocked, 0);
            }
        }
    }

    /// <summary>
    /// Gracefully completes the writing side of the stream.
    /// Equivalent to using <see cref="WriteAsync(System.ReadOnlyMemory{byte},bool,System.Threading.CancellationToken)"/> with <c>completeWrites: true</c>.
    /// </summary>
    /// <remarks>
    /// Corresponds to an empty <see href="https://www.rfc-editor.org/rfc/rfc9000.html#frame-stream">STREAM</see> frame with <c>FIN</c> flag set to <c>true</c>.
    /// </remarks>
    public void CompleteWrites()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);

        // Nothing to complete, the writing side is already closed.
        if (_sendTcs.IsCompleted)
        {
            return;
        }

        if (NetEventSource.Log.IsEnabled())
        {
            NetEventSource.Info(this, $"{this} Completing writes.");
        }
        unsafe
        {
            ThrowHelper.ThrowIfMsQuicError(MsQuicApi.Api.StreamShutdown(
                _handle,
                QUIC_STREAM_SHUTDOWN_FLAGS.GRACEFUL,
                default),
                "StreamShutdown failed");
        }
    }

    private int HandleEventStartComplete(ref START_COMPLETE_DATA data)
    {
        Debug.Assert(_decrementStreamCapacity is not null);

        _id = unchecked((long)data.ID);
        if (StatusSucceeded(data.Status))
        {
            _decrementStreamCapacity(Type);

            if (data.PeerAccepted != 0)
            {
                _startedTcs.TrySetResult();
            }
            // If PeerAccepted == 0, we will later receive PEER_ACCEPTED event, which will complete the _startedTcs.
        }
        else
        {
            if (ThrowHelper.TryGetStreamExceptionForMsQuicStatus(data.Status, out Exception? exception))
            {
                _startedTcs.TrySetException(exception);
            }
        }

        _decrementStreamCapacity = null;
        return QUIC_STATUS_SUCCESS;
    }
    private unsafe int HandleEventReceive(ref RECEIVE_DATA data)
    {
        ulong totalCopied = (ulong)_receiveBuffers.CopyFrom(
            new ReadOnlySpan<QUIC_BUFFER>(data.Buffers, (int)data.BufferCount),
            (int)data.TotalBufferLength,
            data.Flags.HasFlag(QUIC_RECEIVE_FLAGS.FIN));

        if (totalCopied < data.TotalBufferLength)
        {
            Volatile.Write(ref _receivedNeedsEnable, 1);
        }

        _receiveTcs.TrySetResult();

        data.TotalBufferLength = totalCopied;
        return (_receiveBuffers.HasCapacity() && Interlocked.CompareExchange(ref _receivedNeedsEnable, 0, 1) == 1) ? QUIC_STATUS_CONTINUE : QUIC_STATUS_SUCCESS;
    }
    private int HandleEventSendComplete(ref SEND_COMPLETE_DATA data)
    {
        // Release buffer and unlock.
        _sendBuffers.Reset();
        Volatile.Write(ref _sendLocked, 0);

        // There might be stored exception from when we held the lock.
        Exception? exception = Volatile.Read(ref _sendException);
        if (exception is not null)
        {
            _sendTcs.TrySetException(exception);
        }
        if (data.Canceled == 0)
        {
            _sendTcs.TrySetResult();
        }
        // If Canceled != 0, we either aborted write, received PEER_RECEIVE_ABORTED or will receive SHUTDOWN_COMPLETE(ConnectionClose) later, all of which completes the _sendTcs.
        return QUIC_STATUS_SUCCESS;
    }
    private int HandleEventPeerSendShutdown()
    {
        // Same as RECEIVE with FIN flag. Remember that no more RECEIVE events will come.
        // Don't set the task to its final state yet, but wait for all the buffered data to get consumed first.
        _receiveBuffers.SetFinal();
        _receiveTcs.TrySetResult();
        return QUIC_STATUS_SUCCESS;
    }
    private int HandleEventPeerSendAborted(ref PEER_SEND_ABORTED_DATA data)
    {
        _receiveTcs.TrySetException(ThrowHelper.GetStreamAbortedException((long)data.ErrorCode));
        return QUIC_STATUS_SUCCESS;
    }
    private int HandleEventPeerReceiveAborted(ref PEER_RECEIVE_ABORTED_DATA data)
    {
        _sendTcs.TrySetException(ThrowHelper.GetStreamAbortedException((long)data.ErrorCode));
        return QUIC_STATUS_SUCCESS;
    }
    private int HandleEventSendShutdownComplete(ref SEND_SHUTDOWN_COMPLETE_DATA data)
    {
        if (data.Graceful != 0)
        {
            _sendTcs.TrySetResult(final: true);
        }
        // If Graceful == 0, we either aborted write, received PEER_RECEIVE_ABORTED or will receive SHUTDOWN_COMPLETE(ConnectionClose) later, all of which completes the _sendTcs.
        return QUIC_STATUS_SUCCESS;
    }
    private int HandleEventShutdownComplete(ref SHUTDOWN_COMPLETE_DATA data)
    {
        if (data.ConnectionShutdown != 0)
        {
            bool shutdownByApp = data.ConnectionShutdownByApp != 0;
            bool closedRemotely = data.ConnectionClosedRemotely != 0;
            Exception exception = (shutdownByApp, closedRemotely) switch
            {
                // It's remote shutdown by app, peer's side called QuicConnection.CloseAsync, throw QuicError.ConnectionAborted.
                (shutdownByApp: true, closedRemotely: true) => ThrowHelper.GetConnectionAbortedException((long)data.ConnectionErrorCode),
                // It's local shutdown by app, this side called QuicConnection.CloseAsync, throw QuicError.OperationAborted.
                (shutdownByApp: true, closedRemotely: false) => ThrowHelper.GetOperationAbortedException(),
                // It's remote shutdown by transport, we received a CONNECTION_CLOSE frame with a QUIC transport error code, throw error based on the status.
                (shutdownByApp: false, closedRemotely: true) => ThrowHelper.GetExceptionForMsQuicStatus(data.ConnectionCloseStatus, (long)data.ConnectionErrorCode),
                // It's local shutdown by transport, most likely due to a timeout, throw error based on the status.
                (shutdownByApp: false, closedRemotely: false) => ThrowHelper.GetExceptionForMsQuicStatus(data.ConnectionCloseStatus, (long)data.ConnectionErrorCode),
            };
            _startedTcs.TrySetException(exception);
            _receiveTcs.TrySetException(exception);
            _sendTcs.TrySetException(exception);
        }
        _startedTcs.TrySetException(ThrowHelper.GetOperationAbortedException());
        _shutdownTcs.TrySetResult();
        return QUIC_STATUS_SUCCESS;
    }
    private int HandleEventPeerAccepted()
    {
        _startedTcs.TrySetResult();
        return QUIC_STATUS_SUCCESS;
    }

    private int HandleStreamEvent(ref QUIC_STREAM_EVENT streamEvent)
        => streamEvent.Type switch
        {
            QUIC_STREAM_EVENT_TYPE.START_COMPLETE => HandleEventStartComplete(ref streamEvent.START_COMPLETE),
            QUIC_STREAM_EVENT_TYPE.RECEIVE => HandleEventReceive(ref streamEvent.RECEIVE),
            QUIC_STREAM_EVENT_TYPE.SEND_COMPLETE => HandleEventSendComplete(ref streamEvent.SEND_COMPLETE),
            QUIC_STREAM_EVENT_TYPE.PEER_SEND_SHUTDOWN => HandleEventPeerSendShutdown(),
            QUIC_STREAM_EVENT_TYPE.PEER_SEND_ABORTED => HandleEventPeerSendAborted(ref streamEvent.PEER_SEND_ABORTED),
            QUIC_STREAM_EVENT_TYPE.PEER_RECEIVE_ABORTED => HandleEventPeerReceiveAborted(ref streamEvent.PEER_RECEIVE_ABORTED),
            QUIC_STREAM_EVENT_TYPE.SEND_SHUTDOWN_COMPLETE => HandleEventSendShutdownComplete(ref streamEvent.SEND_SHUTDOWN_COMPLETE),
            QUIC_STREAM_EVENT_TYPE.SHUTDOWN_COMPLETE => HandleEventShutdownComplete(ref streamEvent.SHUTDOWN_COMPLETE),
            QUIC_STREAM_EVENT_TYPE.PEER_ACCEPTED => HandleEventPeerAccepted(),
            _ => QUIC_STATUS_SUCCESS
        };

#pragma warning disable CS3016
    [UnmanagedCallersOnly(CallConvs = new Type[] { typeof(CallConvCdecl) })]
#pragma warning restore CS3016
    private static unsafe int NativeCallback(QUIC_HANDLE* stream, void* context, QUIC_STREAM_EVENT* streamEvent)
    {
        GCHandle stateHandle = GCHandle.FromIntPtr((IntPtr)context);

        // Check if the instance hasn't been collected.
        if (!stateHandle.IsAllocated || stateHandle.Target is not QuicStream instance)
        {
            if (NetEventSource.Log.IsEnabled())
            {
                NetEventSource.Error(null, $"Received event {streamEvent->Type} for [strm][{(nint)stream:X11}] while stream is already disposed");
            }
            return QUIC_STATUS_INVALID_STATE;
        }

        try
        {
            // Process the event.
            if (NetEventSource.Log.IsEnabled())
            {
                NetEventSource.Info(instance, $"{instance} Received event {streamEvent->Type} {streamEvent->ToString()}");
            }
            return instance.HandleStreamEvent(ref *streamEvent);
        }
        catch (Exception ex)
        {
            if (NetEventSource.Log.IsEnabled())
            {
                NetEventSource.Error(instance, $"{instance} Exception while processing event {streamEvent->Type}: {ex}");
            }
            return QUIC_STATUS_INTERNAL_ERROR;
        }
    }

    /// <summary>
    /// If the read side is not fully consumed, i.e.: <see cref="ReadsClosed"/> is not completed and/or <see cref="ReadAsync(Memory{byte}, CancellationToken)"/> hasn't returned <c>0</c>,
    /// dispose will abort the read side with provided <see cref="QuicConnectionOptions.DefaultStreamErrorCode"/>.
    /// If the write side hasn't been closed, it'll be closed gracefully as if <see cref="CompleteWrites"/> was called.
    /// Finally, all resources associated with the stream will be released.
    /// </summary>
    /// <returns>A task that represents the asynchronous dispose operation.</returns>
    public override async ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref _disposed, true))
        {
            return;
        }

        if (NetEventSource.Log.IsEnabled())
        {
            NetEventSource.Info(this, $"{this} Disposing.");
        }

        // If the stream wasn't started successfully, gracelessly abort it.
        if (!_startedTcs.IsCompletedSuccessfully)
        {
            // Check if the stream has been shut down and if not, shut it down.
            StreamShutdown(QUIC_STREAM_SHUTDOWN_FLAGS.ABORT | QUIC_STREAM_SHUTDOWN_FLAGS.IMMEDIATE, _defaultErrorCode);
        }
        else
        {
            // Abort the read side and complete the write side if that side hasn't been completed yet.
            if (!_receiveTcs.IsCompleted)
            {
                StreamShutdown(QUIC_STREAM_SHUTDOWN_FLAGS.ABORT_RECEIVE, _defaultErrorCode);
            }
            if (!_sendTcs.IsCompleted)
            {
                StreamShutdown(QUIC_STREAM_SHUTDOWN_FLAGS.GRACEFUL, default);
            }
        }

        // Wait for SHUTDOWN_COMPLETE, the last event, so that all resources can be safely released.
        if (_shutdownTcs.TryInitialize(out ValueTask valueTask, this))
        {
            await valueTask.ConfigureAwait(false);
        }
        Debug.Assert(_startedTcs.IsCompleted);
        _handle.Dispose();

        void StreamShutdown(QUIC_STREAM_SHUTDOWN_FLAGS flags, long errorCode)
        {
            int status = MsQuicApi.Api.StreamShutdown(
                _handle,
                flags,
                (ulong)errorCode);
            if (StatusFailed(status))
            {
                if (NetEventSource.Log.IsEnabled())
                {
                    NetEventSource.Error(this, $"{this} StreamShutdown({flags}) failed: {ThrowHelper.GetErrorMessageForStatus(status)}.");
                }
            }
            else
            {
                if (flags.HasFlag(QUIC_STREAM_SHUTDOWN_FLAGS.ABORT_RECEIVE) && !_receiveTcs.IsCompleted)
                {
                    _receiveTcs.TrySetException(ThrowHelper.GetOperationAbortedException(SR.net_quic_reading_aborted));
                }
                if (flags.HasFlag(QUIC_STREAM_SHUTDOWN_FLAGS.ABORT_SEND) && !_sendTcs.IsCompleted)
                {
                    _sendTcs.TrySetException(ThrowHelper.GetOperationAbortedException(SR.net_quic_writing_aborted));
                }
            }
        }
    }
}
