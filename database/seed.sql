INSERT INTO posts (slug, title, excerpt, body, cover, author, created_at) VALUES
('streamlined-safeguarding-processes-with-oneren',
 'Creating Streamlined Safeguarding Processes with OneRen',
 'How one organisation moved its safeguarding checks out of spreadsheets and into a single workflow.',
 'Safeguarding is not a task you do once a year. It is a process that touches every volunteer, coach and member of staff.

Before moving to one system, the team tracked checks in three different spreadsheets. Renewal dates were missed, and nobody could say at a glance who was cleared to work with young people.

After the switch every role has a clear checklist, reminders go out automatically 30 days before a check expires, and administrators see a single dashboard with the status of everyone.

The result: no missed renewals in twelve months and roughly six hours less admin per week.',
 '/images/blog-1.jpg', 'Nexcent Team', strftime('%Y-%m-%dT%H:%M:%fZ', 'now', '-6 days')),

('safeguarding-responsibilities-and-how-to-manage-them',
 'What are your safeguarding responsibilities and how can you manage them?',
 'A plain-language overview of what membership organisations are expected to do, and where software helps.',
 'Every organisation that works with the public has a duty of care. In practice this means knowing who is on your team, what training they have had and when it expires.

Start with a simple register: name, role, check date, expiry date. Then decide who is responsible for reviewing it and how often.

The second step is communication. Members and volunteers should know how to raise a concern and who will receive it.

Finally, keep a record. A clear audit trail protects both the people you serve and the people who serve.',
 '/images/blog-2.jpg', 'Nexcent Team', strftime('%Y-%m-%dT%H:%M:%fZ', 'now', '-5 days')),

('revamping-the-membership-model-with-triathlon-australia',
 'Revamping the Membership Model with Triathlon Australia',
 'What changed when a national sports body simplified its membership tiers and moved renewals online.',
 'Triathlon Australia had more than a dozen membership types, many of them overlapping. Members found the choice confusing and clubs spent hours explaining the differences.

The new model has three clear tiers. Pricing is shown up front, and renewals are handled online with automatic reminders.

Within one season, online renewals grew from under half to almost nine in ten, and support requests about fees dropped sharply.',
 '/images/blog-3.jpg', 'Nexcent Team', strftime('%Y-%m-%dT%H:%M:%fZ', 'now', '-4 days')),

('five-ways-to-grow-your-membership-income',
 'Five ways to grow your membership income without raising fees',
 'Practical ideas that clubs and associations can apply this season.',
 'Raising fees is the hardest way to grow income. These five ideas usually work better.

1. Make renewing effortless. Every extra click costs you members.
2. Offer an annual option with a small discount.
3. Sell events and merchandise in the same checkout.
4. Ask lapsed members why they left, then act on the answers.
5. Show members what their money achieves.',
 '/images/blog-1.jpg', 'Nexcent Team', strftime('%Y-%m-%dT%H:%M:%fZ', 'now', '-3 days')),

('running-events-without-the-spreadsheet',
 'Running events without the spreadsheet',
 'Bookings, payments and attendance lists in one place.',
 'If your event bookings live in an inbox and a spreadsheet, you already know the problems: double bookings, unpaid places and last-minute lists.

Put the event page, the payment and the attendance list in one tool. Members book in two minutes, you see the numbers live, and the check-in list is ready on the day.',
 '/images/blog-2.jpg', 'Nexcent Team', strftime('%Y-%m-%dT%H:%M:%fZ', 'now', '-2 days')),

('what-members-expect-from-your-website',
 'What members expect from your website',
 'Mobile-first, fast, and with a login that just works.',
 'Most members will visit your site on a phone, usually to do one thing: renew, book or pay.

Keep those paths short. Make sure the login remembers them, the page loads quickly and the confirmation arrives immediately.

Everything else — news, history, committee lists — can wait one tap further away.',
 '/images/blog-3.jpg', 'Nexcent Team', strftime('%Y-%m-%dT%H:%M:%fZ', 'now', '-1 days'));

INSERT INTO faqs (question, answer, position) VALUES
('How do I create an account?', 'Press Sign up, enter your name, email and password. You are signed in right away.', 1),
('Can I import my existing members?', 'Yes. Upload a CSV file and we map the columns for you.', 2),
('Which payment methods are supported?', 'Cards and bank transfers. More methods are coming.', 3),
('Is my data safe?', 'Passwords are hashed with PBKDF2-SHA256, sessions can be revoked at any time, and data is encrypted in transit.', 4),
('How do I cancel my plan?', 'Open Settings, choose Billing and press Cancel plan.', 5);
