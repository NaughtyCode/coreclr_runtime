// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.IO;
using System.Text;
using System.Diagnostics;
using System.Collections.Generic;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging.Abstractions;
using Moq;
using Xunit;

namespace Microsoft.Extensions.Logging.Test
{
    public class LoggerFactoryTest
    {
        [ConditionalFact(typeof(PlatformDetection), nameof(PlatformDetection.IsReflectionEmitSupported))]
        public void AddProvider_ThrowsAfterDisposed()
        {
            var factory = new LoggerFactory();
            factory.Dispose();

            Assert.Throws<ObjectDisposedException>(() => ((ILoggerFactory)factory).AddProvider(CreateProvider()));
        }

        [Fact]
        public void AddProvider_ThrowsIfProviderIsNull()
        {
            var factory = new LoggerFactory();

            Assert.Throws<ArgumentNullException>(() => ((ILoggerFactory)factory).AddProvider(null));
        }

        [Fact]
        public void CreateLogger_ThrowsAfterDisposed()
        {
            var factory = new LoggerFactory();
            factory.Dispose();
            Assert.Throws<ObjectDisposedException>(() => factory.CreateLogger("d"));
        }

        private class TestLoggerFactory : LoggerFactory
        {
            public bool Disposed => CheckDisposed();
        }

        [Fact]
        public void Dispose_MultipleCallsNoop()
        {
            var factory = new TestLoggerFactory();
            factory.Dispose();
            Assert.True(factory.Disposed);
            factory.Dispose();
        }

        // Moq heavily utilizes RefEmit, which does not work on most aot workloads
        [ConditionalFact(typeof(PlatformDetection), nameof(PlatformDetection.IsReflectionEmitSupported))]
        public void Dispose_ProvidersAreDisposed()
        {
            // Arrange
            var factory = new LoggerFactory();
            var disposableProvider1 = CreateProvider();
            var disposableProvider2 = CreateProvider();

            factory.AddProvider(disposableProvider1);
            factory.AddProvider(disposableProvider2);

            // Act
            factory.Dispose();

            // Assert
            Mock.Get<IDisposable>(disposableProvider1)
                    .Verify(p => p.Dispose(), Times.Once());
            Mock.Get<IDisposable>(disposableProvider2)
                     .Verify(p => p.Dispose(), Times.Once());
        }

        private static ILoggerProvider CreateProvider()
        {
            var disposableProvider = new Mock<ILoggerProvider>();
            disposableProvider.As<IDisposable>()
                  .Setup(p => p.Dispose());
            return disposableProvider.Object;
        }

        // Moq heavily utilizes RefEmit, which does not work on most aot workloads
        [ConditionalFact(typeof(PlatformDetection), nameof(PlatformDetection.IsReflectionEmitSupported))]
        public void Dispose_ThrowException_SwallowsException()
        {
            // Arrange
            var factory = new LoggerFactory();
            var throwingProvider = new Mock<ILoggerProvider>();
            throwingProvider.As<IDisposable>()
                .Setup(p => p.Dispose())
                .Throws<Exception>();

            factory.AddProvider(throwingProvider.Object);

            // Act
            factory.Dispose();

            // Assert
            throwingProvider.As<IDisposable>()
                .Verify(p => p.Dispose(), Times.Once());
        }

        private static string GetActivityLogString(ActivityTrackingOptions options)
        {
            Activity activity = Activity.Current;
            if (activity == null)
            {
                return string.Empty;
            }

            StringBuilder sb = new StringBuilder();
            if ((options & ActivityTrackingOptions.SpanId) != 0)
            {
                sb.Append($"SpanId:{activity.GetSpanId()}");
            }

            if ((options & ActivityTrackingOptions.TraceId) != 0)
            {
                sb.Append(sb.Length > 0 ? $", TraceId:{activity.GetTraceId()}" : $"TraceId:{activity.GetTraceId()}");
            }

            if ((options & ActivityTrackingOptions.ParentId) != 0)
            {
                sb.Append(sb.Length > 0 ? $", ParentId:{activity.GetParentId()}" : $"ParentId:{activity.GetParentId()}");
            }

            if ((options & ActivityTrackingOptions.TraceState) != 0)
            {
                sb.Append(sb.Length > 0 ? $", TraceState:{activity.TraceStateString}" : $"TraceState:{activity.TraceStateString}");
            }

            if ((options & ActivityTrackingOptions.TraceFlags) != 0)
            {
                sb.Append(sb.Length > 0 ? $", TraceFlags:{activity.ActivityTraceFlags}" : $"TraceFlags:{activity.ActivityTraceFlags}");
            }

            return sb.ToString();
        }

        [Theory]
        [InlineData(ActivityTrackingOptions.SpanId)]
        [InlineData(ActivityTrackingOptions.TraceId)]
        [InlineData(ActivityTrackingOptions.ParentId)]
        [InlineData(ActivityTrackingOptions.TraceState)]
        [InlineData(ActivityTrackingOptions.TraceFlags)]
        [InlineData(ActivityTrackingOptions.SpanId | ActivityTrackingOptions.TraceId)]
        [InlineData(ActivityTrackingOptions.SpanId | ActivityTrackingOptions.ParentId)]
        [InlineData(ActivityTrackingOptions.SpanId | ActivityTrackingOptions.TraceState)]
        [InlineData(ActivityTrackingOptions.SpanId | ActivityTrackingOptions.TraceFlags)]
        [InlineData(ActivityTrackingOptions.TraceId | ActivityTrackingOptions.ParentId)]
        [InlineData(ActivityTrackingOptions.TraceId | ActivityTrackingOptions.TraceState)]
        [InlineData(ActivityTrackingOptions.TraceId | ActivityTrackingOptions.TraceFlags)]
        [InlineData(ActivityTrackingOptions.ParentId | ActivityTrackingOptions.TraceState)]
        [InlineData(ActivityTrackingOptions.ParentId | ActivityTrackingOptions.TraceFlags)]
        [InlineData(ActivityTrackingOptions.TraceState | ActivityTrackingOptions.TraceFlags)]
        [InlineData(ActivityTrackingOptions.SpanId | ActivityTrackingOptions.TraceId | ActivityTrackingOptions.ParentId)]
        [InlineData(ActivityTrackingOptions.SpanId | ActivityTrackingOptions.TraceId | ActivityTrackingOptions.TraceState)]
        [InlineData(ActivityTrackingOptions.SpanId | ActivityTrackingOptions.TraceId | ActivityTrackingOptions.TraceFlags)]
        [InlineData(ActivityTrackingOptions.SpanId | ActivityTrackingOptions.ParentId | ActivityTrackingOptions.TraceState)]
        [InlineData(ActivityTrackingOptions.SpanId | ActivityTrackingOptions.ParentId | ActivityTrackingOptions.TraceFlags)]
        [InlineData(ActivityTrackingOptions.SpanId | ActivityTrackingOptions.TraceState | ActivityTrackingOptions.TraceFlags)]
        [InlineData(ActivityTrackingOptions.TraceId | ActivityTrackingOptions.ParentId | ActivityTrackingOptions.TraceState)]
        [InlineData(ActivityTrackingOptions.TraceId | ActivityTrackingOptions.ParentId | ActivityTrackingOptions.TraceFlags)]
        [InlineData(ActivityTrackingOptions.TraceId | ActivityTrackingOptions.TraceState | ActivityTrackingOptions.TraceFlags)]
        [InlineData(ActivityTrackingOptions.SpanId | ActivityTrackingOptions.TraceId | ActivityTrackingOptions.ParentId | ActivityTrackingOptions.TraceState)]
        [InlineData(ActivityTrackingOptions.SpanId | ActivityTrackingOptions.TraceId | ActivityTrackingOptions.ParentId | ActivityTrackingOptions.TraceFlags)]
        [InlineData(ActivityTrackingOptions.TraceId | ActivityTrackingOptions.ParentId | ActivityTrackingOptions.TraceState | ActivityTrackingOptions.TraceFlags)]
        [InlineData(ActivityTrackingOptions.SpanId | ActivityTrackingOptions.TraceId | ActivityTrackingOptions.ParentId | ActivityTrackingOptions.TraceState | ActivityTrackingOptions.TraceFlags)]
        public void TestActivityIds(ActivityTrackingOptions options)
        {
            var loggerProvider = new ExternalScopeLoggerProvider();

            var loggerFactory = LoggerFactory.Create(builder =>
            {
                builder
                .Configure(o => o.ActivityTrackingOptions = options)
                .AddProvider(loggerProvider);
            });

            var logger = loggerFactory.CreateLogger("Logger");

            Activity activity = new Activity("ScopeActivity");
            activity.AddBaggage("baggageTestKey1", "baggageTestValue");
            activity.AddTag("tagTestKey", "tagTestValue");
            activity.Start();
            string activity1String = GetActivityLogString(options);
            string activity2String;

            using (logger.BeginScope("Scope 1"))
            {
                logger.LogInformation("Message 1");
                Activity b = new Activity("ScopeActivity");
                b.Start();
                activity2String = GetActivityLogString(options);

                using (logger.BeginScope("Scope 2"))
                {
                    logger.LogInformation("Message 2");
                }
                b.Stop();
            }
            activity.Stop();

            Assert.Equal(activity1String, loggerProvider.LogText[1]);
            Assert.Equal(activity2String, loggerProvider.LogText[4]);
            Assert.Equal(7, loggerProvider.LogText.Count); // Ensure that Baggage and Tags aren't added.
        }

        [Fact]
        public void TestInvalidActivityTrackingOptions()
        {
            Assert.Throws<ArgumentException>(() =>
                LoggerFactory.Create(builder => { builder.Configure(o => o.ActivityTrackingOptions = (ActivityTrackingOptions)0xFF00); })
            );
        }

        [Fact]
        public void TestActivityTrackingOptions_ShouldAddBaggageItemsAsNewScope_WhenBaggageOptionIsSet()
        {
            var loggerProvider = new ExternalScopeLoggerProvider();

            var loggerFactory = LoggerFactory.Create(builder =>
            {
                builder
                    .Configure(o => o.ActivityTrackingOptions = ActivityTrackingOptions.Baggage)
                    .AddProvider(loggerProvider);
            });

            var logger = loggerFactory.CreateLogger("Logger");

            Activity activity = new Activity("ScopeActivity");
            activity.AddBaggage("testKey1", null);
            activity.AddBaggage("testKey2", string.Empty);
            activity.AddBaggage("testKey3", "testValue");
            activity.Start();

            logger.LogInformation("Message1");

            activity.Stop();

            foreach (string s in loggerProvider.LogText)
            {
                System.Console.WriteLine(s);
            }

            Assert.Equal("Message1", loggerProvider.LogText[0]);
            Assert.Equal("testKey3:testValue, testKey2:, testKey1:", loggerProvider.LogText[2]);
        }

        [Fact]
        public void TestActivityTrackingOptions_ShouldAddTagsAsNewScope_WhenTagsOptionIsSet()
        {
            var loggerProvider = new ExternalScopeLoggerProvider();

            var loggerFactory = LoggerFactory.Create(builder =>
            {
                builder
                    .Configure(o => o.ActivityTrackingOptions = ActivityTrackingOptions.TraceId | ActivityTrackingOptions.Tags)
                    .AddProvider(loggerProvider);
            });

            var logger = loggerFactory.CreateLogger("Logger");

            Activity activity = new Activity("ScopeActivity");
            activity.AddTag("testKey1", null);
            activity.AddTag("testKey2", string.Empty);
            activity.AddTag("testKey3", "testValue");
            activity.AddTag("testKey4", new Dummy());
            activity.Start();

            logger.LogInformation("Message1");

            activity.Stop();

            Assert.Equal("Message1", loggerProvider.LogText[0]);
            Assert.Equal("testKey1:, testKey2:, testKey3:testValue, testKey4:DummyToString", loggerProvider.LogText[2]);
        }

        [Fact]
        public void TestActivityTrackingOptions_ShouldAddTagsAndBaggageAsOneScopeAndTraceIdAsOtherScope_WhenTagsBaggageAndTraceIdOptionAreSet()
        {
            var loggerProvider = new ExternalScopeLoggerProvider();

            var loggerFactory = LoggerFactory.Create(builder =>
            {
                builder
                    .Configure(o => o.ActivityTrackingOptions = ActivityTrackingOptions.TraceId | ActivityTrackingOptions.Baggage | ActivityTrackingOptions.Tags)
                    .AddProvider(loggerProvider);
            });

            var logger = loggerFactory.CreateLogger("Logger");

            Activity activity = new Activity("ScopeActivity");
            activity.AddTag("testTagKey1", "testTagValue");
            activity.AddBaggage("testBaggageKey1", "testBaggageValue");
            activity.Start();
            logger.LogInformation("Message1");
            string traceIdActivityLogString = GetActivityLogString(ActivityTrackingOptions.TraceId);
            activity.Stop();

            Assert.Equal("Message1", loggerProvider.LogText[0]);
            Assert.Equal(traceIdActivityLogString, loggerProvider.LogText[1]);
            Assert.Equal("testTagKey1:testTagValue", loggerProvider.LogText[2]);
            Assert.Equal("testBaggageKey1:testBaggageValue", loggerProvider.LogText[3]);
        }

        [Fact]
        public void TestActivityTrackingOptions_ShouldAddNewTagAndBaggageItemsAtRuntime_WhenTagsAndBaggageOptionAreSetAndWithNestedScopes()
        {
            var loggerProvider = new ExternalScopeLoggerProvider();

            var loggerFactory = LoggerFactory.Create(builder =>
            {
                builder
                    .Configure(o => o.ActivityTrackingOptions = ActivityTrackingOptions.Baggage | ActivityTrackingOptions.Tags)
                    .AddProvider(loggerProvider);
            });

            var logger = loggerFactory.CreateLogger("Logger");

            Activity activity = new Activity("ScopeActivity");
            activity.Start();

            // Add baggage and tag items before the first log entry.
            activity.AddTag("MyTagKey1", "1");
            activity.AddBaggage("MyBaggageKey1", "1");

            // Log a message, this should create any cached objects.
            logger.LogInformation("Message1");

            // Start the first scope, add some more items and log.
            using (logger.BeginScope("Scope1"))
            {
                activity.AddTag("MyTagKey2", "2");
                activity.AddBaggage("MyBaggageKey2", "2");
                logger.LogInformation("Message2");

                // Add two additional scopes and also replace some tag and baggage items.
                using (logger.BeginScope("Scope2"))
                {
                    activity.AddTag("MyTagKey3", "3");
                    activity.AddBaggage("MyBaggageKey3", "3");

                    using (logger.BeginScope("Scope3"))
                    {
                        activity.SetTag("MyTagKey3", "4");
                        activity.SetBaggage("MyBaggageKey3", "4");
                        logger.LogInformation("Message3");
                    }
                }

                // Along with this message we expect all baggage and tags items
                // as well as the Scope1 but not the Scope2 and Scope3.
                logger.LogInformation("Message4");

                activity.Stop();
            }

            Assert.Equal("Message1", loggerProvider.LogText[0]);
            Assert.Equal("MyTagKey1:1", loggerProvider.LogText[2]);
            Assert.Equal("MyBaggageKey1:1", loggerProvider.LogText[3]);

            Assert.Equal("Message2", loggerProvider.LogText[4]);
            Assert.Equal("MyTagKey1:1, MyTagKey2:2", loggerProvider.LogText[6]);
            Assert.Equal("MyBaggageKey2:2, MyBaggageKey1:1", loggerProvider.LogText[7]);
            Assert.Equal("Scope1", loggerProvider.LogText[8]);

            Assert.Equal("Message3", loggerProvider.LogText[9]);
            Assert.Equal("MyTagKey1:1, MyTagKey2:2, MyTagKey3:4", loggerProvider.LogText[11]);
            Assert.Equal("MyBaggageKey3:4, MyBaggageKey2:2, MyBaggageKey1:1", loggerProvider.LogText[12]);
            Assert.Equal("Scope1", loggerProvider.LogText[13]);
            Assert.Equal("Scope2", loggerProvider.LogText[14]);
            Assert.Equal("Scope3", loggerProvider.LogText[15]);

            Assert.Equal("Message4", loggerProvider.LogText[16]);
            Assert.Equal("MyTagKey1:1, MyTagKey2:2, MyTagKey3:4", loggerProvider.LogText[18]);
            Assert.Equal("MyBaggageKey3:4, MyBaggageKey2:2, MyBaggageKey1:1", loggerProvider.LogText[19]);
            Assert.Equal("Scope1", loggerProvider.LogText[20]);
        }

        [Fact]
        public void TestActivityTrackingOptions_ShouldNotAddAdditionalScope_WhenTagsBaggageOptionAreSetButTagsAndBaggageAreEmpty()
        {
            var loggerProvider = new ExternalScopeLoggerProvider();

            var loggerFactory = LoggerFactory.Create(builder =>
            {
                builder
                    .Configure(o => o.ActivityTrackingOptions = ActivityTrackingOptions.TraceId | ActivityTrackingOptions.Baggage | ActivityTrackingOptions.Tags)
                    .AddProvider(loggerProvider);
            });

            var logger = loggerFactory.CreateLogger("Logger");

            Activity activity = new Activity("ScopeActivity");
            activity.Start();
            logger.LogInformation("Message1");
            string traceIdActivityLogString = GetActivityLogString(ActivityTrackingOptions.TraceId);
            activity.Stop();

            Assert.Equal("Message1", loggerProvider.LogText[0]);
            Assert.Equal(traceIdActivityLogString, loggerProvider.LogText[1]);
            Assert.Equal(2, loggerProvider.LogText.Count); // Ensure that the additional scopes for tags and baggage aren't added.
        }

        [Fact]
        public void BaggageFormattedOutput()
        {
            var loggerProvider = new ExternalScopeLoggerWithFormatterProvider();
            var loggerFactory = LoggerFactory.Create(builder =>
            {
                builder
                    .Configure(o => o.ActivityTrackingOptions = ActivityTrackingOptions.TraceId | ActivityTrackingOptions.Baggage | ActivityTrackingOptions.Tags)
                    .AddProvider(loggerProvider);
            });

            var logger = loggerFactory.CreateLogger("Logger");

            Activity activity = new Activity("ScopeActivity");
            activity.Start();

            activity.AddTag("Tag1", "1");
            activity.AddBaggage("Baggage1", "1");

            using (logger.BeginScope("Scope1"))
            {
                activity.AddTag("Tag2", "2");
                activity.AddBaggage("Baggage2", "2");
                logger.LogInformation("Inside Scope Info!");
            }
            activity.Stop();

            string[] loggerOutput = new string[]
            {
                $"Inside Scope Info!",
                $"[TraceId, {activity.GetTraceId()}]",
                $"[Tag1, 1]",
                $"[Tag2, 2]",
                $"[Baggage2, 2]",
                $"[Baggage1, 1]",
                $"Scope1",
            };
            Assert.Equal(loggerOutput, loggerProvider.LogText);
        }

        [Fact]
        public void CallsSetScopeProvider_OnSupportedProviders()
        {
            var loggerProvider = new ExternalScopeLoggerProvider();
            var loggerFactory = new LoggerFactory(new[] { loggerProvider });

            var logger = loggerFactory.CreateLogger("Logger");

            using (logger.BeginScope("Scope"))
            {
                using (logger.BeginScope("Scope2"))
                {
                    logger.LogInformation("Message");
                }
            }
            logger.LogInformation("Message2");

            Assert.Equal(loggerProvider.LogText,
                new[]
                {
                    "Message",
                    "Scope",
                    "Scope2",
                    "Message2",
                });
            Assert.NotNull(loggerProvider.ScopeProvider);
            Assert.Equal(0, loggerProvider.BeginScopeCalledTimes);
        }

        [Fact]
        public void BeginScope_ReturnsExternalSourceTokenDirectly()
        {
            var loggerProvider = new ExternalScopeLoggerProvider();
            var loggerFactory = new LoggerFactory(new[] { loggerProvider });

            var logger = loggerFactory.CreateLogger("Logger");

            var scope = logger.BeginScope("Scope");
            Assert.StartsWith(loggerProvider.ScopeProvider.GetType().FullName, scope.GetType().FullName);
        }

        [Fact]
        public void BeginScope_ReturnsInternalSourceTokenDirectly()
        {
            var loggerProvider = new InternalScopeLoggerProvider();
            var loggerFactory = new LoggerFactory(new[] { loggerProvider });
            var logger = loggerFactory.CreateLogger("Logger");
            var scope = logger.BeginScope("Scope");
            Assert.Contains("LoggerExternalScopeProvider+Scope", scope.GetType().FullName);
        }

        [Fact]
        public void BeginScope_ReturnsCompositeToken_ForMultipleLoggers()
        {
            var loggerProvider = new ExternalScopeLoggerProvider();
            var loggerProvider2 = new InternalScopeLoggerProvider();
            var loggerFactory = new LoggerFactory(new ILoggerProvider[] { loggerProvider, loggerProvider2 });

            var logger = loggerFactory.CreateLogger("Logger");

            using (logger.BeginScope("Scope"))
            {
                using (logger.BeginScope("Scope2"))
                {
                    logger.LogInformation("Message");
                }
            }
            logger.LogInformation("Message2");

            Assert.Equal(loggerProvider.LogText,
                new[]
                {
                    "Message",
                    "Scope",
                    "Scope2",
                    "Message2",
                });

            Assert.Equal(loggerProvider2.LogText,
                new[]
                {
                    "Message",
                    "Scope",
                    "Scope2",
                    "Message2",
                });
        }

        // Moq heavily utilizes RefEmit, which does not work on most aot workloads
        [ConditionalFact(typeof(PlatformDetection), nameof(PlatformDetection.IsReflectionEmitSupported))]
        public void CreateDisposeDisposesInnerServiceProvider()
        {
            var disposed = false;
            var provider = new Mock<ILoggerProvider>();
            provider.Setup(p => p.Dispose()).Callback(() => disposed = true);

            var factory = LoggerFactory.Create(builder => builder.Services.AddSingleton(_ => provider.Object));
            factory.Dispose();

            Assert.True(disposed);
        }

        // Moq heavily utilizes RefEmit, which does not work on most aot workloads
        [ConditionalFact(typeof(PlatformDetection), nameof(PlatformDetection.IsReflectionEmitSupported))]
        public void TestCreateLoggers_NullLoggerIsIgnoredWhenReturnedByProvider()
        {
            // We test this via checking if Scope optimisaion (ie not return scope wrapper but the
            // returned scope directly only one logger) is applied.
            var nullProvider = new Mock<ILoggerProvider>();
            nullProvider.Setup(p => p.CreateLogger(It.IsAny<string>())).Returns(NullLogger.Instance);

            var validProvider = new CustomScopeLoggerProvider();

            var factory = LoggerFactory.Create(builder =>
            {
                builder.AddProvider(nullProvider.Object);
                builder.AddProvider(validProvider);
            });
            var logger = factory.CreateLogger("TestLogger");

            var scope = logger.BeginScope("TestScope");
            Assert.IsType<CustomScopeLoggerProvider.CustomScope>(scope);

            logger.LogInformation("Test message");
            Assert.Equal(1, validProvider.LogText.Count);
        }

        private class CustomScopeLoggerProvider : ILoggerProvider, ILogger
        {
            public List<string> LogText { get; set; } = new List<string>();

            public void Dispose() { }

            public ILogger CreateLogger(string categoryName) => this;

            public void Log<TState>(LogLevel logLevel, EventId eventId, TState state, Exception exception, Func<TState, Exception, string> formatter) => LogText.Add(formatter(state, exception));

            public bool IsEnabled(LogLevel logLevel) => true;

            public IDisposable BeginScope<TState>(TState state) => new CustomScope();

            internal class CustomScope : IDisposable
            {
                public void Dispose() { }
            }
        }

        private class InternalScopeLoggerProvider : ILoggerProvider, ILogger
        {
            private IExternalScopeProvider _scopeProvider = new LoggerExternalScopeProvider();
            public List<string> LogText { get; set; } = new List<string>();

            public void Dispose()
            {
            }

            public ILogger CreateLogger(string categoryName)
            {
                return this;
            }

            public void Log<TState>(LogLevel logLevel, EventId eventId, TState state, Exception exception, Func<TState, Exception, string> formatter)
            {
                LogText.Add(formatter(state, exception));
                _scopeProvider.ForEachScope((scope, builder) => builder.Add(scope.ToString()), LogText);
            }

            public bool IsEnabled(LogLevel logLevel)
            {
                return true;
            }

            public IDisposable BeginScope<TState>(TState state)
            {
                return _scopeProvider.Push(state);
            }
        }

        private class ExternalScopeLoggerProvider : ILoggerProvider, ISupportExternalScope, ILogger
        {
            public void SetScopeProvider(IExternalScopeProvider scopeProvider)
            {
                ScopeProvider = scopeProvider;
            }

            public IExternalScopeProvider ScopeProvider { get; set; }
            public int BeginScopeCalledTimes { get; set; }
            public List<string> LogText { get; set; } = new List<string>();
            public void Dispose()
            {
            }

            public ILogger CreateLogger(string categoryName)
            {
                return this;
            }

            public void Log<TState>(LogLevel logLevel, EventId eventId, TState state, Exception exception, Func<TState, Exception, string> formatter)
            {
                LogText.Add(formatter(state, exception));

                // Notice that other ILoggers maybe not call "ToString()" on the scope but enumerate it and this isn't covered by this implementation.
                // E.g. the SimpleConsoleFormatter calls "ToString()" like it's done here but the "JsonConsoleFormatter" enumerates a scope
                // if the Scope is of type IEnumerable<KeyValuePair<string, object>>.
                ScopeProvider.ForEachScope((scope, builder) => builder.Add(scope.ToString()), LogText);
            }

            public bool IsEnabled(LogLevel logLevel)
            {
                return true;
            }

            public IDisposable BeginScope<TState>(TState state)
            {
                BeginScopeCalledTimes++;
                return null;
            }
        }

        // Support formatting IEnumerable<KeyValuePair<string, object?>> scopes
        private class ExternalScopeLoggerWithFormatterProvider : ILoggerProvider, ISupportExternalScope, ILogger
        {
            public void SetScopeProvider(IExternalScopeProvider scopeProvider)
            {
                ScopeProvider = scopeProvider;
            }

            public IExternalScopeProvider ScopeProvider { get; set; }

            public int BeginScopeCalledTimes { get; set; }

            public List<string> LogText { get; set; } = new List<string>();

            public void Dispose() { }

            public ILogger CreateLogger(string categoryName) => this;

            public void Log<TState>(LogLevel logLevel, EventId eventId, TState state, Exception exception, Func<TState, Exception, string> formatter)
            {
                LogText.Add(formatter(state, exception));

                ScopeProvider.ForEachScope((scope, builder) =>
                {
                    if (scope is IEnumerable<KeyValuePair<string, object>> scopeItems)
                    {
                        foreach (KeyValuePair<string, object> item in scopeItems)
                        {
                            builder.Add(item.ToString());
                        }
                    }
                    else
                    {
                        builder.Add(scope.ToString());
                    }
                }, LogText);
            }

            public bool IsEnabled(LogLevel logLevel) => true;

            public IDisposable BeginScope<TState>(TState state)
            {
                BeginScopeCalledTimes++;
                return null;
            }
        }

        private class Dummy
        {
            public override string ToString()
            {
                return "DummyToString";
            }
        }
    }

    internal static class ActivityExtensions
    {
        public static string GetSpanId(this Activity activity)
        {
            return activity.IdFormat switch
            {
                ActivityIdFormat.Hierarchical => activity.Id,
                ActivityIdFormat.W3C => activity.SpanId.ToHexString(),
                _ => null,
            } ?? string.Empty;
        }

        public static string GetTraceId(this Activity activity)
        {
            return activity.IdFormat switch
            {
                ActivityIdFormat.Hierarchical => activity.RootId,
                ActivityIdFormat.W3C => activity.TraceId.ToHexString(),
                _ => null,
            } ?? string.Empty;
        }

        public static string GetParentId(this Activity activity)
        {
            return activity.IdFormat switch
            {
                ActivityIdFormat.Hierarchical => activity.ParentId,
                ActivityIdFormat.W3C => activity.ParentSpanId.ToHexString(),
                _ => null,
            } ?? string.Empty;
        }
    }
}
