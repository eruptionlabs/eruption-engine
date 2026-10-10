// GameObject, Component, MonoBehaviour e Transform (implementacao propria).
// Os objetos vivem no lado gerenciado; a ponte (Eruption.Compat.Ponte)
// espelha nome e transformacao no host nativo a cada quadro.
using System;
using System.Collections.Generic;

namespace UnityEngine
{
    public enum Space { World = 0, Self = 1 }

    public class Object
    {
        public string name { get; set; } = "";
        internal bool destruido;

        public static void Destroy(Object obj) { Destroy(obj, 0f); }
        public static void Destroy(Object obj, float t)
        {
            if (obj is GameObject go) Eruption.Compat.Ponte.Destruir(go);
            else if (obj is Component c) Eruption.Compat.Ponte.DestruirComponente(c);
        }
        public static void DontDestroyOnLoad(Object obj) { }
        public static implicit operator bool(Object o) => o != null && !o.destruido;
        public override string ToString() => name + " (" + GetType().Name + ")";
    }

    public class Component : Object
    {
        public GameObject gameObject { get; internal set; }
        public Transform transform => gameObject.transform;
        public string tag { get; set; } = "Untagged";
        public T GetComponent<T>() where T : Component => gameObject.GetComponent<T>();
        public Component GetComponent(Type t) => gameObject.GetComponent(t);
        public T[] GetComponents<T>() where T : Component => gameObject.GetComponents<T>();
    }

    public class Behaviour : Component
    {
        bool _enabled = true;
        public bool enabled
        {
            get => _enabled;
            set
            {
                if (_enabled == value) return;
                _enabled = value;
                Eruption.Compat.Ponte.MudouHabilitado(this);
            }
        }
        public bool isActiveAndEnabled => _enabled && gameObject.activeInHierarchy;
    }

    public class MonoBehaviour : Behaviour
    {
        public void Invoke(string metodo, float tempo) { Eruption.Compat.Ponte.Agendar(this, metodo, tempo); }
        public Coroutine StartCoroutine(System.Collections.IEnumerator rotina)
        {
            Eruption.Compat.Ponte.IniciarCorrotina(this, rotina);
            return new Coroutine();
        }
        public void print(object mensagem) { Debug.Log(mensagem); }
    }

    public sealed class GameObject : Object
    {
        readonly List<Component> _componentes = new List<Component>();
        bool _ativo = true;
        internal uint noHost;

        public Transform transform { get; }

        public GameObject() : this("GameObject") { }
        public GameObject(string nome)
        {
            name = nome;
            transform = new Transform();
            transform.gameObject = this;
            transform.name = nome;
            _componentes.Add(transform);
            Eruption.Compat.Ponte.Registrar(this);
        }
        public GameObject(string nome, params Type[] componentes) : this(nome)
        {
            foreach (Type t in componentes) AddComponent(t);
        }

        public bool activeSelf => _ativo;
        public bool activeInHierarchy => _ativo && (transform.parent == null || transform.parent.gameObject.activeInHierarchy);
        public void SetActive(bool ativo) { _ativo = ativo; }
        public string tag { get; set; } = "Untagged";
        public int layer { get; set; }

        public T AddComponent<T>() where T : Component => (T)AddComponent(typeof(T));
        public Component AddComponent(Type t)
        {
            var c = (Component)Activator.CreateInstance(t, true);
            c.gameObject = this;
            c.name = name;
            _componentes.Add(c);
            Eruption.Compat.Ponte.ComponenteAdicionado(c);
            return c;
        }
        public T GetComponent<T>() where T : Component => (T)GetComponent(typeof(T));
        public Component GetComponent(Type t)
        {
            foreach (Component c in _componentes) if (t.IsInstanceOfType(c) && !c.destruido) return c;
            return null;
        }
        public T[] GetComponents<T>() where T : Component
        {
            var r = new List<T>();
            foreach (Component c in _componentes) if (c is T x && !c.destruido) r.Add(x);
            return r.ToArray();
        }
        internal List<Component> Componentes => _componentes;
    }

    public class Transform : Component, System.Collections.IEnumerable
    {
        Vector3 _pos = Vector3.zero;
        Quaternion _rot = Quaternion.identity;
        Vector3 _escala = Vector3.one;
        Transform _pai;
        readonly List<Transform> _filhos = new List<Transform>();
        internal bool sujo = true;

        public Vector3 localPosition { get => _pos; set { _pos = value; sujo = true; } }
        public Quaternion localRotation { get => _rot; set { _rot = Quaternion.Normalize(value); sujo = true; } }
        public Vector3 localScale { get => _escala; set { _escala = value; sujo = true; } }
        public Vector3 localEulerAngles { get => _rot.eulerAngles; set => localRotation = Quaternion.Euler(value); }

        public Transform parent { get => _pai; set => SetParent(value); }
        public Transform root => _pai == null ? this : _pai.root;
        public int childCount => _filhos.Count;
        public Transform GetChild(int i) => _filhos[i];
        public void SetParent(Transform p) { SetParent(p, true); }
        public void SetParent(Transform p, bool manterMundo)
        {
            Vector3 pm = position;
            Quaternion rm = rotation;
            _pai?._filhos.Remove(this);
            _pai = p;
            _pai?._filhos.Add(this);
            if (manterMundo) { position = pm; rotation = rm; }
            sujo = true;
        }

        public Vector3 lossyScale => _pai == null ? _escala : Vector3.Scale(_pai.lossyScale, _escala);
        public Quaternion rotation
        {
            get => _pai == null ? _rot : _pai.rotation * _rot;
            set => localRotation = _pai == null ? value : Quaternion.Inverse(_pai.rotation) * value;
        }
        public Vector3 position
        {
            get => _pai == null ? _pos : _pai.TransformPoint(_pos);
            set => localPosition = _pai == null ? value : _pai.InverseTransformPoint(value);
        }
        public Vector3 eulerAngles { get => rotation.eulerAngles; set => rotation = Quaternion.Euler(value); }
        public Vector3 forward { get => rotation * Vector3.forward; set => rotation = Quaternion.LookRotation(value); }
        public Vector3 right => rotation * Vector3.right;
        public Vector3 up => rotation * Vector3.up;

        public Vector3 TransformPoint(Vector3 p) => position + rotation * Vector3.Scale(lossyScale, p);
        public Vector3 InverseTransformPoint(Vector3 p)
        {
            Vector3 l = Quaternion.Inverse(rotation) * (p - position);
            Vector3 s = lossyScale;
            return new Vector3(s.x != 0 ? l.x / s.x : 0, s.y != 0 ? l.y / s.y : 0, s.z != 0 ? l.z / s.z : 0);
        }
        public Vector3 TransformDirection(Vector3 d) => rotation * d;
        public Vector3 InverseTransformDirection(Vector3 d) => Quaternion.Inverse(rotation) * d;

        public void Translate(Vector3 d) { Translate(d, Space.Self); }
        public void Translate(Vector3 d, Space espaco)
        {
            if (espaco == Space.World) position += d;
            else position += TransformDirection(d);
        }
        public void Translate(float x, float y, float z) { Translate(new Vector3(x, y, z)); }
        public void Rotate(Vector3 euler) { Rotate(euler, Space.Self); }
        public void Rotate(Vector3 euler, Space espaco)
        {
            Quaternion q = Quaternion.Euler(euler);
            if (espaco == Space.Self) localRotation = localRotation * q;
            else rotation = q * rotation;
        }
        public void Rotate(float x, float y, float z) { Rotate(new Vector3(x, y, z)); }
        public void Rotate(Vector3 eixo, float angulo) { Rotate(eixo, angulo, Space.Self); }
        public void Rotate(Vector3 eixo, float angulo, Space espaco)
        {
            Quaternion q = Quaternion.AngleAxis(angulo, eixo);
            if (espaco == Space.Self) localRotation = localRotation * q;
            else rotation = q * rotation;
        }
        public void LookAt(Vector3 alvo) { rotation = Quaternion.LookRotation(alvo - position); }
        public void LookAt(Transform alvo) { LookAt(alvo.position); }

        public System.Collections.IEnumerator GetEnumerator() => _filhos.GetEnumerator();
    }
}
