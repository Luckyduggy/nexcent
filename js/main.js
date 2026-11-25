const $ = (selector, root = document) => root.querySelector(selector);
const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];

const TOKEN_KEY = 'nx_token';
const IN_PAGES = location.pathname.includes('/pages/');
const PAGES = IN_PAGES ? '' : 'pages/';
const HOME = IN_PAGES ? '../index.html' : 'index.html';

const HERO_SLIDES = [
    ['Lessons and insights', 'from 8 years', 'Where to grow your business as a photographer: site or social media?'],
    ['Grow your community', 'in one system', 'Manage members, events and payments without extra tools.'],
    ['Automate renewals', 'and payments', 'Spend less time on admin and more time with your members.']
];

const SUCCESS_MESSAGES = {
    '/subscribe': 'Thanks for subscribing!',
    '/contact': 'Thank you! We will reply within one working day.'
};

const store = {
    get token() { try { return localStorage.getItem(TOKEN_KEY); } catch { return null; } },
    set(token) { try { localStorage.setItem(TOKEN_KEY, token); } catch {} },
    clear() { try { localStorage.removeItem(TOKEN_KEY); } catch {} }
};

function el(tag, props = {}, ...children) {
    const node = document.createElement(tag);
    for (const [name, value] of Object.entries(props)) {
        if (name === 'role' || name.startsWith('aria-') || name.startsWith('data-')) node.setAttribute(name, value);
        else node[name] = value;
    }
    node.append(...children.flat().filter(child => child != null && child !== false));
    return node;
}

function coverImage(className, post) {
    const node = el('div', { className, role: 'img', 'aria-label': post.title });
    node.style.backgroundImage = `url("${encodeURI(post.cover)}")`;
    return node;
}

const formatDate = iso => new Date(iso).toLocaleDateString('en-GB', { day: 'numeric', month: 'short', year: 'numeric' });

const describeError = error => (error.data?.fields ? Object.values(error.data.fields).join(' ') : error.message);

async function api(path, { method = 'GET', body, auth = true } = {}) {
    const headers = {};
    if (body !== undefined) headers['Content-Type'] = 'application/json';
    if (auth && store.token) headers.Authorization = `Bearer ${store.token}`;

    let response;
    try {
        response = await fetch('/api' + path, { method, headers, body: body === undefined ? undefined : JSON.stringify(body) });
    } catch {
        throw Object.assign(new Error('Cannot reach the server. Please try again.'), { status: 0 });
    }

    const data = await response.json().catch(() => null);
    if (!response.ok) {
        throw Object.assign(new Error(data?.error || `Request failed (${response.status})`), { status: response.status, data });
    }
    return data;
}

async function logout() {
    await api('/logout', { method: 'POST' }).catch(() => null);
    store.clear();
    location.href = HOME;
}

function initMenu() {
    const burger = $('.burger');
    burger?.addEventListener('click', () => burger.setAttribute('aria-expanded', $('.menu').classList.toggle('open')));
}

function initHeroSlider() {
    const title = $('.hero h1');
    if (!title) return;

    const text = $('.hero p');
    const dots = $$('.dots button');
    let current = 0;
    let timer;

    const show = index => {
        const [heading, accent, description] = HERO_SLIDES[index];
        current = index;
        title.innerHTML = `${heading} <em>${accent}</em>`;
        text.textContent = description;
        dots.forEach((dot, dotIndex) => dot.classList.toggle('on', dotIndex === index));
    };
    const restart = () => {
        clearInterval(timer);
        timer = setInterval(() => show((current + 1) % HERO_SLIDES.length), 6000);
    };

    dots.forEach((dot, index) => dot.addEventListener('click', () => { show(index); restart(); }));
    restart();
}

function animateCounter(element) {
    const target = Number(element.dataset.n);
    const start = performance.now();
    const step = now => {
        const progress = Math.min((now - start) / 1400, 1);
        element.textContent = Math.round(target * progress).toLocaleString('en-US');
        if (progress < 1) requestAnimationFrame(step);
    };
    requestAnimationFrame(step);
}

function initCounters() {
    const observer = new IntersectionObserver(entries => {
        for (const entry of entries.filter(entry => entry.isIntersecting)) {
            observer.unobserve(entry.target);
            animateCounter(entry.target);
        }
    });
    $$('[data-n]').forEach(counter => observer.observe(counter));
}

async function submitForm(form) {
    const next = form.nextElementSibling;
    const message = next?.classList.contains('msg') ? next : form.insertAdjacentElement('afterend', el('p', { className: 'msg', role: 'status' }));
    const button = $('button', form);
    const show = (isSuccess, text) => {
        message.className = isSuccess ? 'msg' : 'msg err';
        message.textContent = text;
    };

    button.disabled = true;
    try {
        const data = await api(form.dataset.api, { method: 'POST', body: Object.fromEntries(new FormData(form)), auth: false });
        if (data.token) {
            store.set(data.token);
            show(true, 'Welcome!');
            location.href = `${PAGES}dashboard.html`;
            return;
        }
        form.reset();
        show(true, SUCCESS_MESSAGES[form.dataset.api] || 'Done!');
    } catch (error) {
        show(false, describeError(error));
    } finally {
        button.disabled = false;
    }
}

function initForms() {
    $$('form[data-api]').forEach(form => form.addEventListener('submit', event => {
        event.preventDefault();
        submitForm(form);
    }));
}

async function initAuthHeader() {
    const authBox = $('.auth');
    if (!authBox || !store.token) return;

    try {
        const { user } = await api('/me');
        const logoutLink = el('a', { className: 'btn sm', href: '#', textContent: 'Logout' });
        logoutLink.addEventListener('click', event => {
            event.preventDefault();
            logout();
        });
        authBox.replaceChildren(el('a', { className: 'login', href: `${PAGES}dashboard.html`, textContent: user.name.split(' ')[0] }), logoutLink);
    } catch (error) {
        if (error.status === 401) store.clear();
    }
}

function blogCard(post) {
    const link = el('a', { className: 'more', href: `${PAGES}post.html?slug=${encodeURIComponent(post.slug)}`, textContent: 'Read more →' });
    return el('article', {}, coverImage('bi bi-photo', post), el('div', { className: 'bb' }, el('h3', { textContent: post.title }), link));
}

function initBlog() {
    $$('.blog[data-posts]').forEach(async container => {
        try {
            const { items } = await api(`/posts?limit=${Number(container.dataset.posts) || 6}`, { auth: false });
            if (items.length > 0) container.replaceChildren(...items.map(blogCard));
        } catch {}
    });
}

async function initFaq() {
    const container = $('#faq-list');
    if (!container) return;

    try {
        const { items } = await api('/faqs', { auth: false });
        const entries = items.map(item => el('details', {}, el('summary', { textContent: item.question }), el('p', { textContent: item.answer })));
        if (entries.length > 0) container.replaceChildren(...entries);
    } catch {}
}

initMenu();
initHeroSlider();
initCounters();
initForms();
initAuthHeader();
initBlog();
initFaq();
