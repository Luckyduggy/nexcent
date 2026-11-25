(() => {
    const container = $('#post');
    if (!container) return;

    const backLink = el('a', { className: 'more', href: 'blog.html', textContent: '← Back to blog' });

    const showPost = post => {
        document.title = `${post.title} | Nexcent`;
        container.replaceChildren(
            el('h1', { textContent: post.title }),
            el('p', { className: 'post-meta', textContent: `${post.author} · ${formatDate(post.created_at)}` }),
            coverImage('post-cover', post),
            ...post.body.split(/\n{2,}/).map(text => text.trim()).filter(Boolean).map(text => el('p', { textContent: text })),
            backLink
        );
    };

    const showError = error => container.replaceChildren(
        el('h1', { textContent: error.status === 404 ? 'Post not found' : 'Something went wrong' }),
        el('p', { textContent: error.message }),
        backLink
    );

    const slug = new URLSearchParams(location.search).get('slug');
    const request = slug
        ? api(`/posts/${encodeURIComponent(slug)}`, { auth: false })
        : Promise.reject(Object.assign(new Error('This post does not exist.'), { status: 404 }));
    request.then(showPost).catch(showError);
})();
