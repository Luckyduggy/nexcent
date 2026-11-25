(() => {
    const root = $('#dash');
    if (!root) return;

    const PAGE_SIZE = 25;
    const DAY_MS = 24 * 60 * 60 * 1000;

    const flashBox = el('p', { className: 'msg', role: 'status' });
    const panel = el('div', { className: 'panel' });
    let currentUser;
    let flashTimer;
    let refreshTimer;

    const flash = (text, isError = false) => {
        flashBox.className = isError ? 'msg err' : 'msg';
        flashBox.textContent = text;
        clearTimeout(flashTimer);
        flashTimer = setTimeout(() => { flashBox.textContent = ''; }, 5000);
    };

    const formatDateTime = iso => new Date(iso).toLocaleString('en-GB', { dateStyle: 'medium', timeStyle: 'short' });

    const formatDuration = milliseconds => {
        const seconds = Math.max(0, Math.floor(milliseconds / 1000));
        if (seconds < 60) return `${seconds}s`;
        if (seconds < 3600) return `${Math.floor(seconds / 60)}m ${seconds % 60}s`;
        return `${Math.floor(seconds / 3600)}h ${Math.floor((seconds % 3600) / 60)}m`;
    };

    const sessionExpired = error => {
        if (error.status !== 401) return false;
        store.clear();
        location.replace('login.html');
        return true;
    };

    const button = (label, onClick, className = 'btn-s') => {
        const node = el('button', { type: 'button', className, textContent: label });
        node.addEventListener('click', async () => {
            node.disabled = true;
            try { await onClick(); } catch (error) { flash(describeError(error), true); } finally { node.disabled = false; }
        });
        return node;
    };

    const deleteButton = (question, path, reload) => button('Delete', async () => {
        if (!confirm(question)) return;
        await api(path, { method: 'DELETE' });
        await reload();
    }, 'btn-s danger');

    const pill = (text, isGood) => el('span', { className: isGood ? 'pill ok' : 'pill warn', textContent: text });

    const row = (className, ...cells) => el('tr', { className: className || '' }, cells.map(cell => el('td', {}, cell)));

    const table = (headings, rows) => el('div', { className: 'tbl' }, el('table', {},
        el('thead', {}, el('tr', {}, headings.map(heading => el('th', { textContent: heading })))),
        el('tbody', {}, rows)));

    function pagedList(path, headings, makeRow, emptyText) {
        const element = el('div');
        let items = [];
        let total = 0;

        const draw = () => {
            const content = [
                items.length > 0
                    ? table(headings, items.map(item => makeRow(item, () => load(true))))
                    : el('p', { className: 'muted', textContent: emptyText }),
                el('p', { className: 'muted', textContent: `${total} total` })
            ];
            if (items.length < total) content.splice(1, 0, button(`Load more (${items.length} of ${total})`, () => load(false)));
            element.replaceChildren(...content);
        };

        const load = async startOver => {
            const page = await api(`${path}${path.includes('?') ? '&' : '?'}limit=${PAGE_SIZE}&offset=${startOver ? 0 : items.length}`);
            items = startOver ? page.items : items.concat(page.items);
            total = page.total;
            draw();
        };

        return { element, load };
    }

    function showList(path, headings, makeRow, emptyText) {
        const list = pagedList(path, headings, makeRow, emptyText);
        panel.replaceChildren(list.element);
        return list.load(true);
    }

    const profileCard = user => el('div', { className: 'dash-card' },
        el('h2', { textContent: 'Your account' }),
        el('dl', { className: 'kv' }, [
            ['Name', user.name],
            ['Email', user.email],
            ['Role', user.role],
            ['Member since', formatDate(user.created_at)]
        ].flatMap(([label, value]) => [el('dt', { textContent: label }), el('dd', { textContent: value })])),
        button('Sign out', logout));

    function dailyBars(label, rows, fromDate, dayCount) {
        const start = Date.UTC(Number(fromDate.slice(0, 4)), Number(fromDate.slice(5, 7)) - 1, Number(fromDate.slice(8, 10)));
        const days = Array.from({ length: dayCount }, (_, index) => new Date(start + index * DAY_MS).toISOString().slice(0, 10));
        const countByDay = Object.fromEntries(rows.map(item => [item.d, item.c]));
        const values = days.map(day => countByDay[day] || 0);
        const maxValue = Math.max(1, ...values);

        const bars = values.map((value, index) => {
            const bar = el('i', { title: `${days[index]}: ${value}` });
            bar.style.height = `${Math.max(value > 0 ? 10 : 3, (value / maxValue) * 100)}%`;
            return bar;
        });
        const sum = values.reduce((total, value) => total + value, 0);

        return el('div', { className: 'bars-row' }, el('span', { className: 'bars-label', textContent: `${label}: ${sum}` }), el('div', { className: 'bars' }, bars));
    }

    function benchmarkCard() {
        const output = el('div', { className: 'bench-out muted' });
        const select = el('select', { 'aria-label': 'Threads' }, [1, 2, 4, 8, 16].map(count =>
            el('option', { value: count, textContent: `${count} thread${count > 1 ? 's' : ''}`, selected: count === 4 })));

        const runButton = button('Run benchmark', async () => {
            output.textContent = 'Running…';
            const result = await api(`/admin/benchmark?n=${select.value}`);
            output.replaceChildren(
                el('p', {
                    textContent: `${result.threads} parallel threads on ${result.cpus} CPU core(s): one hash ${result.single_ms} ms · ` +
                        `sequential ${result.sequential_estimate_ms} ms · parallel ${result.parallel_wall_ms} ms → speed-up ×${result.speedup}`
                }),
                el('p', { textContent: `Per-thread time: ${result.per_thread_ms.join(', ')} ms. Speed-up cannot exceed the number of CPU cores.` })
            );
        }, 'btn sm');

        return el('div', { className: 'dash-card' },
            el('h2', { textContent: 'Thread benchmark' }),
            el('p', {
                className: 'muted',
                textContent: 'The server hashes passwords (PBKDF2, ~70 ms of pure CPU) in N parallel pthreads and compares the time with doing it one by one.'
            }),
            el('div', { className: 'row' }, select, runButton),
            output);
    }

    async function showOverview() {
        const statsBox = el('div', { className: 'dash-stats' });
        const chartsBox = el('div', { className: 'dash-card' });
        const serverBox = el('div', { className: 'dash-card' });
        panel.replaceChildren(statsBox, chartsBox, serverBox, benchmarkCard());

        const refresh = async () => {
            const { counts, daily, server, workers } = await api('/admin/stats');

            statsBox.replaceChildren(...[
                ['Users', counts.users],
                ['Subscribers', counts.subscribers],
                ['Messages', counts.messages],
                ['Unread', counts.unread],
                ['Posts', counts.posts],
                ['Active sessions', counts.sessions]
            ].map(([label, value]) => el('div', { className: 'dash-stat' }, el('strong', { textContent: value }), el('span', { textContent: label }))));

            chartsBox.replaceChildren(
                el('h2', { textContent: `Last ${daily.days} days` }),
                dailyBars('New users', daily.users, daily.from, daily.days),
                dailyBars('Subscribers', daily.subscribers, daily.from, daily.days),
                dailyBars('Messages', daily.messages, daily.from, daily.days)
            );

            serverBox.replaceChildren(
                el('h2', { textContent: 'Server processes' }),
                el('p', {
                    className: 'muted',
                    textContent: `This request was served by process ${server.served_by} · ${server.threads} active thread(s) · ` +
                        `${server.cpus} CPU core(s). Refreshes every 4 s.`
                }),
                table(['PID', 'Worker', 'Uptime', 'Requests handled', 'Threads', 'Heartbeat'], workers.map(worker => {
                    const isCurrent = worker.pid === server.served_by;
                    return row(
                        isCurrent ? 'me' : '',
                        isCurrent ? `${worker.pid} (this request)` : String(worker.pid),
                        `#${worker.worker_id}`,
                        formatDuration(server.now - worker.started_at),
                        String(worker.requests),
                        String(worker.threads),
                        `${formatDuration(server.now - worker.last_seen)} ago`
                    );
                }))
            );
        };

        await refresh();
        refreshTimer = setInterval(() => { if (!document.hidden) refresh().catch(() => null); }, 4000);
    }

    const messageRow = (message, reload) => row(
        message.is_read ? '' : 'unread',
        formatDateTime(message.created_at),
        el('span', {}, el('strong', { textContent: message.name }), el('br'), el('a', { href: `mailto:${message.email}`, textContent: message.email })),
        el('div', { className: 'pre', textContent: message.message }),
        pill(message.is_read ? 'read' : 'new', message.is_read),
        el('div', { className: 'act' },
            button(message.is_read ? 'Mark unread' : 'Mark read', async () => {
                await api(`/admin/messages/${message.id}`, { method: 'PATCH', body: { is_read: !message.is_read } });
                await reload();
            }),
            deleteButton('Delete this message?', `/admin/messages/${message.id}`, reload))
    );

    const subscriberRow = (subscriber, reload) => row(
        '',
        formatDateTime(subscriber.created_at),
        subscriber.email,
        deleteButton('Remove this subscriber?', `/admin/subscribers/${subscriber.id}`, reload)
    );

    function userRow(user, reload) {
        const isSelf = user.id === currentUser.id;
        const select = el('select', { disabled: isSelf, 'aria-label': 'Role' },
            ['user', 'admin'].map(role => el('option', { value: role, textContent: role, selected: user.role === role })));

        select.addEventListener('change', async () => {
            try {
                await api(`/admin/users/${user.id}`, { method: 'PATCH', body: { role: select.value } });
                flash('Role updated.');
            } catch (error) {
                flash(describeError(error), true);
            }
            await reload();
        });

        return row(
            '',
            String(user.id),
            user.name,
            user.email,
            select,
            formatDateTime(user.created_at),
            isSelf
                ? el('span', { className: 'muted', textContent: 'you' })
                : deleteButton(`Delete ${user.email} and all their sessions?`, `/admin/users/${user.id}`, reload)
        );
    }

    const activityRow = entry => row(
        entry.event === 'login_fail' ? 'unread' : '',
        formatDateTime(entry.created_at),
        entry.event,
        entry.user_id == null ? '—' : String(entry.user_id),
        entry.ip || '',
        String(entry.pid ?? ''),
        entry.detail || ''
    );

    function postForm(onSaved) {
        const fields = {
            title: el('input', { type: 'text', required: true, maxLength: 160 }),
            excerpt: el('input', { type: 'text', maxLength: 300 }),
            author: el('input', { type: 'text', maxLength: 80, placeholder: 'Nexcent Team' }),
            cover: el('select', {}, ['blog-1.jpg', 'blog-2.jpg', 'blog-3.jpg'].map(name => el('option', { value: `/images/${name}`, textContent: name }))),
            body: el('textarea', { rows: 8, maxLength: 20000, placeholder: 'Separate paragraphs with an empty line.' }),
            published: el('input', { type: 'checkbox', defaultChecked: true })
        };
        const heading = el('h2', { textContent: 'New post' });
        const cancelButton = button('Cancel editing', () => reset());
        cancelButton.hidden = true;
        let editingId = null;

        const element = el('form', { className: 'form post-form' },
            heading,
            el('label', {}, 'Title', fields.title),
            el('label', {}, 'Excerpt', fields.excerpt),
            el('div', { className: 'row' }, el('label', {}, 'Author', fields.author), el('label', {}, 'Cover', fields.cover)),
            el('label', {}, 'Body', fields.body),
            el('label', { className: 'check' }, fields.published, ' Published'),
            el('div', { className: 'row' }, el('button', { className: 'btn' }, 'Save post'), cancelButton));

        const reset = () => {
            editingId = null;
            element.reset();
            heading.textContent = 'New post';
            cancelButton.hidden = true;
        };

        const startEditing = post => {
            editingId = post.id;
            for (const name of ['title', 'excerpt', 'author', 'cover', 'body']) fields[name].value = post[name];
            fields.published.checked = Boolean(post.published);
            heading.textContent = 'Edit post';
            cancelButton.hidden = false;
            element.scrollIntoView({ behavior: 'smooth', block: 'center' });
        };

        element.addEventListener('submit', async event => {
            event.preventDefault();
            const wasEditing = editingId !== null;
            const body = { published: fields.published.checked };
            for (const name of ['title', 'excerpt', 'body', 'author', 'cover']) body[name] = fields[name].value;

            try {
                await api(wasEditing ? `/admin/posts/${editingId}` : '/admin/posts', { method: wasEditing ? 'PUT' : 'POST', body });
                flash(wasEditing ? 'Post updated.' : 'Post created.');
                reset();
                await onSaved();
            } catch (error) {
                flash(describeError(error), true);
            }
        });

        return { element, startEditing, reset, getEditingId: () => editingId };
    }

    async function showPosts() {
        let form;
        const list = pagedList('/admin/posts', ['Title', 'Status', 'Date', ''], (post, reload) => row(
            '',
            post.published ? el('a', { href: `post.html?slug=${encodeURIComponent(post.slug)}`, textContent: post.title }) : post.title,
            pill(post.published ? 'published' : 'draft', post.published),
            formatDate(post.created_at),
            el('div', { className: 'act' },
                button('Edit', async () => form.startEditing(await api(`/admin/posts/${post.id}`))),
                deleteButton(`Delete “${post.title}”?`, `/admin/posts/${post.id}`, async () => {
                    if (form.getEditingId() === post.id) form.reset();
                    await reload();
                }))
        ), 'No posts yet.');

        form = postForm(() => list.load(true));
        panel.replaceChildren(form.element, el('div', { className: 'dash-card' }, el('h2', { textContent: 'All posts' }), list.element));
        await list.load(true);
    }

    const TABS = {
        Overview: showOverview,
        Messages: () => showList('/admin/messages', ['Date', 'From', 'Message', 'Status', ''], messageRow, 'No messages yet. Send one from the Contact page.'),
        Subscribers: () => showList('/admin/subscribers', ['Date', 'Email', ''], subscriberRow, 'No subscribers yet.'),
        Users: () => showList('/admin/users', ['ID', 'Name', 'Email', 'Role', 'Joined', ''], userRow, 'No users.'),
        Posts: showPosts,
        Activity: () => showList('/admin/audit', ['Date', 'Event', 'User ID', 'IP', 'Process', 'Detail'], activityRow, 'No events yet.')
    };

    function tabBar() {
        const bar = el('div', { className: 'tabs', role: 'tablist' });

        const openTab = async name => {
            clearInterval(refreshTimer);
            bar.querySelectorAll('button').forEach(tab => tab.setAttribute('aria-selected', String(tab.textContent === name)));
            panel.replaceChildren(el('p', { className: 'muted', textContent: 'Loading…' }));

            try {
                await TABS[name]();
            } catch (error) {
                if (!sessionExpired(error)) panel.replaceChildren(el('p', { className: 'msg err', textContent: error.message }));
            }
        };

        for (const name of Object.keys(TABS)) {
            const tab = el('button', { type: 'button', role: 'tab', textContent: name });
            tab.addEventListener('click', () => openTab(name));
            bar.append(tab);
        }
        return { element: bar, openTab };
    }

    async function start() {
        if (!store.token) {
            location.replace('login.html');
            return;
        }

        try {
            currentUser = (await api('/me')).user;
        } catch (error) {
            if (!sessionExpired(error)) root.replaceChildren(el('p', { className: 'msg err', textContent: error.message }));
            return;
        }

        $('#who').textContent = `Signed in as ${currentUser.name} (${currentUser.email})`;
        root.replaceChildren(flashBox, profileCard(currentUser));
        if (currentUser.role !== 'admin') return;

        const tabs = tabBar();
        root.append(el('h2', { className: 'admin-h', textContent: 'Administration' }), tabs.element, panel);
        tabs.openTab('Overview');
    }

    start();
})();
