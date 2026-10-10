// Time, Debug, Random, Application e atributos de script (implementacao propria).
using System;

namespace UnityEngine
{
    public static class Time
    {
        public static float deltaTime { get; internal set; }
        public static float unscaledDeltaTime { get; internal set; }
        public static float time { get; internal set; }
        public static float unscaledTime { get; internal set; }
        public static float timeSinceLevelLoad => time;
        public static float fixedDeltaTime { get; set; } = 0.02f;
        public static float fixedTime { get; internal set; }
        public static int frameCount { get; internal set; }
        public static float timeScale { get; set; } = 1f;
        public static float realtimeSinceStartup => unscaledTime;
    }

    public enum LogType { Error = 0, Assert = 1, Warning = 2, Log = 3, Exception = 4 }

    public static class Debug
    {
        public static void Log(object m) { Eruption.Compat.Ponte.Log(LogType.Log, m); }
        public static void Log(object m, Object contexto) { Log(m); }
        public static void LogWarning(object m) { Eruption.Compat.Ponte.Log(LogType.Warning, m); }
        public static void LogWarning(object m, Object contexto) { LogWarning(m); }
        public static void LogError(object m) { Eruption.Compat.Ponte.Log(LogType.Error, m); }
        public static void LogError(object m, Object contexto) { LogError(m); }
        public static void LogException(Exception e) { Eruption.Compat.Ponte.Log(LogType.Exception, e); }
        public static void LogFormat(string f, params object[] a) { Log(string.Format(f, a)); }
        public static void LogWarningFormat(string f, params object[] a) { LogWarning(string.Format(f, a)); }
        public static void LogErrorFormat(string f, params object[] a) { LogError(string.Format(f, a)); }
        public static void Assert(bool c) { if (!c) LogError("Assertion failed"); }
        public static void Assert(bool c, object m) { if (!c) LogError(m); }
        public static void DrawLine(Vector3 a, Vector3 b) { }
        public static void DrawLine(Vector3 a, Vector3 b, Color c) { }
        public static void DrawRay(Vector3 a, Vector3 d) { }
        public static void DrawRay(Vector3 a, Vector3 d, Color c) { }
    }

    public static class Random
    {
        static System.Random _g = new System.Random(0x5eed);
        public static void InitState(int semente) { _g = new System.Random(semente); }
        public static float value => (float)_g.NextDouble();
        public static float Range(float min, float max) => min + (max - min) * value;
        public static int Range(int min, int max) => max <= min ? min : _g.Next(min, max);
        public static Vector3 insideUnitSphere
        {
            get
            {
                Vector3 v;
                do { v = new Vector3(Range(-1f, 1f), Range(-1f, 1f), Range(-1f, 1f)); } while (v.sqrMagnitude > 1f);
                return v;
            }
        }
        public static Vector3 onUnitSphere
        {
            get
            {
                Vector3 v;
                do { v = insideUnitSphere; } while (v.sqrMagnitude < 1e-6f);
                return v.normalized;
            }
        }
        public static Vector2 insideUnitCircle
        {
            get
            {
                Vector2 v;
                do { v = new Vector2(Range(-1f, 1f), Range(-1f, 1f)); } while (v.sqrMagnitude > 1f);
                return v;
            }
        }
        public static Quaternion rotation => Quaternion.Euler(Range(0f, 360f), Range(0f, 360f), Range(0f, 360f));
    }

    public static class Application
    {
        public static bool isPlaying => true;
        public static bool isEditor => false;
        public static int targetFrameRate { get; set; } = -1;
        public static string productName => "Eruption";
        public static void Quit() { }
    }

    // Atributos de script: so' metadado (o inspector e a serializacao ficam com o host).
    [AttributeUsage(AttributeTargets.Field)] public sealed class SerializeField : Attribute { }
    [AttributeUsage(AttributeTargets.Field)] public sealed class HideInInspector : Attribute { }
    [AttributeUsage(AttributeTargets.Field)] public sealed class NonReorderableAttribute : Attribute { }
    [AttributeUsage(AttributeTargets.Field, AllowMultiple = true)]
    public sealed class HeaderAttribute : Attribute { public HeaderAttribute(string h) { } }
    [AttributeUsage(AttributeTargets.Field)]
    public sealed class TooltipAttribute : Attribute { public TooltipAttribute(string t) { } }
    [AttributeUsage(AttributeTargets.Field, AllowMultiple = true)]
    public sealed class SpaceAttribute : Attribute { public SpaceAttribute() { } public SpaceAttribute(float h) { } }
    [AttributeUsage(AttributeTargets.Field)]
    public sealed class RangeAttribute : Attribute { public RangeAttribute(float min, float max) { } }
    [AttributeUsage(AttributeTargets.Field)]
    public sealed class MinAttribute : Attribute { public MinAttribute(float min) { } }
    [AttributeUsage(AttributeTargets.Field)]
    public sealed class TextAreaAttribute : Attribute { public TextAreaAttribute() { } public TextAreaAttribute(int a, int b) { } }
    [AttributeUsage(AttributeTargets.Field)]
    public sealed class MultilineAttribute : Attribute { public MultilineAttribute() { } public MultilineAttribute(int l) { } }
    [AttributeUsage(AttributeTargets.Class)]
    public sealed class AddComponentMenu : Attribute { public AddComponentMenu(string m) { } public AddComponentMenu(string m, int o) { } }
    [AttributeUsage(AttributeTargets.Class, AllowMultiple = true)]
    public sealed class RequireComponent : Attribute
    {
        public RequireComponent(Type t) { } public RequireComponent(Type a, Type b) { } public RequireComponent(Type a, Type b, Type c) { }
    }
    [AttributeUsage(AttributeTargets.Class)] public sealed class DisallowMultipleComponent : Attribute { }
    [AttributeUsage(AttributeTargets.Class)] public sealed class ExecuteInEditMode : Attribute { }
    [AttributeUsage(AttributeTargets.Class)] public sealed class ExecuteAlways : Attribute { }
    [AttributeUsage(AttributeTargets.Class)]
    public sealed class DefaultExecutionOrder : Attribute { public DefaultExecutionOrder(int ordem) { } }
    [AttributeUsage(AttributeTargets.Method)]
    public sealed class ContextMenu : Attribute { public ContextMenu(string n) { } }
}
