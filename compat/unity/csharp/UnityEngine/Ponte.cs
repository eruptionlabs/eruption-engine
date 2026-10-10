// Ponte entre os scripts C# e o host nativo da camada compat.
//
// O host (compat/unity/CSharpHost.cpp) carrega este assembly pelo hostfxr e
// chama os metodos [UnmanagedCallersOnly] abaixo. Daqui para la', so'
// ponteiros de funcao da TabelaHost (log e nos de cena). O ciclo de vida
// segue a ordem publica da API: Awake e OnEnable ao adicionar, Start antes
// do primeiro Update, FixedUpdate em passo fixo, Update, LateUpdate,
// OnDisable e OnDestroy ao destruir.
using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using UnityEngine;

namespace Eruption.Compat
{
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct TabelaHost
    {
        public delegate* unmanaged<int, byte*, void> log;               // nivel 0 info, 1 aviso, 2 erro
        public delegate* unmanaged<byte*, uint, uint> criarNo;          // nome, pai -> no
        public delegate* unmanaged<uint, float*, void> definirTransformacao; // pos(3) rot(4) escala(3)
        public delegate* unmanaged<uint, void> destruirNo;
    }

    public static unsafe class Ponte
    {
        static TabelaHost _host;
        static bool _temHost;
        static readonly List<GameObject> _objetos = new List<GameObject>();
        static readonly List<MonoBehaviour> _comportamentos = new List<MonoBehaviour>();
        static readonly HashSet<MonoBehaviour> _semStart = new HashSet<MonoBehaviour>();
        static readonly List<Assembly> _assemblies = new List<Assembly>();
        static readonly Dictionary<(Type, string), MethodInfo> _metodos = new Dictionary<(Type, string), MethodInfo>();
        static readonly List<(MonoBehaviour, IEnumerator, float)> _corrotinas = new List<(MonoBehaviour, IEnumerator, float)>();
        static readonly List<(MonoBehaviour, string, float)> _agendados = new List<(MonoBehaviour, string, float)>();
        static float _acumuladoFixo;

        // ---- Entradas chamadas pelo host nativo ---------------------------------------

        [UnmanagedCallersOnly]
        public static int Iniciar(TabelaHost* tabela)
        {
            _host = *tabela;
            _temHost = true;
            _assemblies.Add(typeof(Ponte).Assembly);
            return 1;
        }

        [UnmanagedCallersOnly]
        public static int CarregarAssembly(byte* caminho)
        {
            string c = Texto(caminho);
            try
            {
                // Mesmo contexto de carga desta ponte: assim o "UnityEngine" que
                // os scripts referenciam e' este, e nao uma segunda copia.
                AssemblyLoadContext ctx = AssemblyLoadContext.GetLoadContext(typeof(Ponte).Assembly);
                Assembly a = ctx.LoadFromAssemblyPath(c);
                _assemblies.Add(a);
                int n = 0;
                foreach (Type t in a.GetTypes()) if (typeof(MonoBehaviour).IsAssignableFrom(t) && !t.IsAbstract) n++;
                LogNativo(0, "assembly carregado: " + a.GetName().Name + " (" + n + " MonoBehaviour)");
                return n;
            }
            catch (Exception e)
            {
                LogNativo(2, "falha ao carregar " + c + ": " + e.Message);
                return -1;
            }
        }

        [UnmanagedCallersOnly]
        public static uint CriarObjeto(byte* nome) => Id(new GameObject(Texto(nome)));

        [UnmanagedCallersOnly]
        public static int AdicionarComponente(uint objeto, byte* tipo)
        {
            GameObject go = Objeto(objeto);
            Type t = AcharTipo(Texto(tipo));
            if (go == null || t == null || !typeof(Component).IsAssignableFrom(t))
            {
                LogNativo(2, "componente desconhecido: " + Texto(tipo));
                return 0;
            }
            go.AddComponent(t);
            return 1;
        }

        [UnmanagedCallersOnly]
        public static int DefinirCampo(uint objeto, byte* tipo, byte* campo, byte* valor)
        {
            GameObject go = Objeto(objeto);
            Type t = AcharTipo(Texto(tipo));
            Component c = (go != null && t != null) ? go.GetComponent(t) : null;
            if (c == null) return 0;
            string nome = Texto(campo), v = Texto(valor);
            for (Type k = c.GetType(); k != null; k = k.BaseType)
            {
                const BindingFlags f = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly;
                FieldInfo fi = k.GetField(nome, f);
                if (fi != null) { fi.SetValue(c, Converter(v, fi.FieldType)); return 1; }
                PropertyInfo pi = k.GetProperty(nome, f);
                if (pi != null && pi.CanWrite) { pi.SetValue(c, Converter(v, pi.PropertyType)); return 1; }
            }
            LogNativo(2, "campo nao encontrado: " + t.Name + "." + nome);
            return 0;
        }

        [UnmanagedCallersOnly]
        public static void Quadro(float dt)
        {
            Time.deltaTime = dt * Time.timeScale;
            Time.unscaledDeltaTime = dt;
            Time.time += Time.deltaTime;
            Time.unscaledTime += dt;
            Time.frameCount++;

            foreach (MonoBehaviour m in _comportamentos.ToArray())
            {
                if (_semStart.Contains(m) && Ativo(m))
                {
                    _semStart.Remove(m);
                    Chamar(m, "Start");
                }
            }
            _acumuladoFixo += Time.deltaTime;
            while (_acumuladoFixo >= Time.fixedDeltaTime)
            {
                _acumuladoFixo -= Time.fixedDeltaTime;
                Time.fixedTime += Time.fixedDeltaTime;
                foreach (MonoBehaviour m in _comportamentos.ToArray()) if (Ativo(m)) Chamar(m, "FixedUpdate");
            }
            foreach (MonoBehaviour m in _comportamentos.ToArray()) if (Ativo(m)) Chamar(m, "Update");
            AvancarCorrotinas();
            foreach (MonoBehaviour m in _comportamentos.ToArray()) if (Ativo(m)) Chamar(m, "LateUpdate");
            Sincronizar();
        }

        [UnmanagedCallersOnly]
        public static int Descrever(uint objeto, byte* buffer, int tamanho)
        {
            GameObject go = Objeto(objeto);
            string s = go == null ? "<destruido>" : string.Format(CultureInfo.InvariantCulture,
                "{0} pos={1} rot={2} componentes={3}", go.name, go.transform.position,
                go.transform.eulerAngles, go.Componentes.Count - 1);
            byte[] b = System.Text.Encoding.UTF8.GetBytes(s);
            int n = Math.Min(b.Length, tamanho - 1);
            Marshal.Copy(b, 0, (IntPtr)buffer, n);
            buffer[n] = 0;
            return n;
        }

        [UnmanagedCallersOnly]
        public static void Encerrar()
        {
            foreach (GameObject go in _objetos.ToArray()) Destruir(go);
            _objetos.Clear();
        }

        // ---- Usado pelo UnityEngine deste assembly ------------------------------------

        internal static void Registrar(GameObject go)
        {
            _objetos.Add(go);
            if (_temHost && _host.criarNo != null)
            {
                byte[] nome = Utf8(go.name);
                fixed (byte* p = nome) go.noHost = _host.criarNo(p, 0);
            }
        }

        internal static void ComponenteAdicionado(Component c)
        {
            if (c is MonoBehaviour m)
            {
                _comportamentos.Add(m);
                _semStart.Add(m);
                Chamar(m, "Awake");
                if (Ativo(m)) Chamar(m, "OnEnable");
            }
        }

        internal static void MudouHabilitado(Behaviour b)
        {
            if (b is MonoBehaviour m) Chamar(m, m.enabled ? "OnEnable" : "OnDisable");
        }

        internal static void DestruirComponente(Component c)
        {
            if (c.destruido) return;
            if (c is MonoBehaviour m)
            {
                if (Ativo(m)) Chamar(m, "OnDisable");
                Chamar(m, "OnDestroy");
                _comportamentos.Remove(m);
                _semStart.Remove(m);
            }
            c.destruido = true;
        }

        internal static void Destruir(GameObject go)
        {
            if (go.destruido) return;
            foreach (Component c in go.Componentes.ToArray()) DestruirComponente(c);
            go.destruido = true;
            _objetos.Remove(go);
            if (_temHost && _host.destruirNo != null && go.noHost != 0) _host.destruirNo(go.noHost);
        }

        internal static void Agendar(MonoBehaviour m, string metodo, float tempo) { _agendados.Add((m, metodo, Time.time + tempo)); }

        internal static void IniciarCorrotina(MonoBehaviour m, IEnumerator e) { _corrotinas.Add((m, e, 0f)); }

        internal static void Log(LogType tipo, object mensagem)
        {
            int nivel = tipo == LogType.Log ? 0 : (tipo == LogType.Warning ? 1 : 2);
            LogNativo(nivel, mensagem == null ? "Null" : mensagem.ToString());
        }

        // ---- Auxiliares ---------------------------------------------------------------

        static bool Ativo(MonoBehaviour m) => !m.destruido && m.enabled && m.gameObject.activeInHierarchy;

        static void Chamar(MonoBehaviour m, string nome)
        {
            Type t = m.GetType();
            if (!_metodos.TryGetValue((t, nome), out MethodInfo mi))
            {
                for (Type k = t; k != null && k != typeof(MonoBehaviour) && mi == null; k = k.BaseType)
                    mi = k.GetMethod(nome, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly,
                                     null, Type.EmptyTypes, null);
                _metodos[(t, nome)] = mi;
            }
            if (mi == null) return;
            try
            {
                object r = mi.Invoke(m, null);
                if (r is IEnumerator e) IniciarCorrotina(m, e); // Start() pode ser corrotina
            }
            catch (TargetInvocationException e)
            {
                LogNativo(2, t.Name + "." + nome + ": " + (e.InnerException ?? e));
            }
        }

        static void AvancarCorrotinas()
        {
            for (int i = 0; i < _corrotinas.Count; i++)
            {
                var (m, e, ate) = _corrotinas[i];
                if (m.destruido) { _corrotinas.RemoveAt(i--); continue; }
                if (Time.time < ate) continue;
                bool continua;
                try { continua = e.MoveNext(); }
                catch (Exception ex) { LogNativo(2, ex.ToString()); continua = false; }
                if (!continua) { _corrotinas.RemoveAt(i--); continue; }
                float espera = e.Current is WaitForSeconds w ? w.segundos : 0f;
                _corrotinas[i] = (m, e, Time.time + espera);
            }
            for (int i = 0; i < _agendados.Count; i++)
            {
                var (m, metodo, ate) = _agendados[i];
                if (Time.time < ate) continue;
                _agendados.RemoveAt(i--);
                if (!m.destruido) Chamar(m, metodo);
            }
        }

        static void Sincronizar()
        {
            if (!_temHost || _host.definirTransformacao == null) return;
            float* v = stackalloc float[10];
            foreach (GameObject go in _objetos)
            {
                Transform t = go.transform;
                if (!t.sujo || go.noHost == 0) continue;
                t.sujo = false;
                Vector3 p = t.localPosition, s = t.localScale;
                Quaternion q = t.localRotation;
                v[0] = p.x; v[1] = p.y; v[2] = p.z;
                v[3] = q.x; v[4] = q.y; v[5] = q.z; v[6] = q.w;
                v[7] = s.x; v[8] = s.y; v[9] = s.z;
                _host.definirTransformacao(go.noHost, v);
            }
        }

        static void LogNativo(int nivel, string s)
        {
            if (!_temHost || _host.log == null) { Console.WriteLine(s); return; }
            byte[] b = Utf8(s);
            fixed (byte* p = b) _host.log(nivel, p);
        }

        static byte[] Utf8(string s)
        {
            byte[] b = System.Text.Encoding.UTF8.GetBytes(s + "\0");
            return b;
        }

        static string Texto(byte* p) => Marshal.PtrToStringUTF8((IntPtr)p) ?? "";

        static uint Id(GameObject go) => (uint)(_objetos.IndexOf(go) + 1);
        static GameObject Objeto(uint id)
        {
            int i = (int)id - 1;
            return i >= 0 && i < _objetos.Count ? _objetos[i] : null;
        }

        static Type AcharTipo(string nome)
        {
            foreach (Assembly a in _assemblies)
            {
                Type t = a.GetType(nome);
                if (t != null) return t;
                foreach (Type k in a.GetTypes()) if (k.Name == nome) return k;
            }
            return null;
        }

        static object Converter(string v, Type t)
        {
            CultureInfo ci = CultureInfo.InvariantCulture;
            if (t == typeof(string)) return v;
            if (t == typeof(float)) return float.Parse(v, ci);
            if (t == typeof(double)) return double.Parse(v, ci);
            if (t == typeof(int)) return int.Parse(v, ci);
            if (t == typeof(bool)) return v == "true" || v == "1";
            if (t.IsEnum) return Enum.Parse(t, v, true);
            string[] p = v.Split(',');
            float F(int i) => i < p.Length ? float.Parse(p[i], ci) : 0f;
            if (t == typeof(Vector3)) return new Vector3(F(0), F(1), F(2));
            if (t == typeof(Vector2)) return new Vector2(F(0), F(1));
            if (t == typeof(Vector4)) return new Vector4(F(0), F(1), F(2), F(3));
            if (t == typeof(Quaternion)) return Quaternion.Euler(F(0), F(1), F(2));
            if (t == typeof(Color)) return new Color(F(0), F(1), F(2), p.Length > 3 ? F(3) : 1f);
            return Convert.ChangeType(v, t, ci);
        }
    }
}

namespace UnityEngine
{
    public class YieldInstruction { }
    public sealed class WaitForSeconds : YieldInstruction
    {
        internal readonly float segundos;
        public WaitForSeconds(float s) { segundos = s; }
    }
    public sealed class WaitForEndOfFrame : YieldInstruction { }
    public sealed class WaitForFixedUpdate : YieldInstruction { }
    public sealed class Coroutine : YieldInstruction { }
}
