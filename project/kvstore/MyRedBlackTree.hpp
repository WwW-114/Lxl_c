#ifndef MYREDBLACKTREE_HPP
#define MYREDBLACKTREE_HPP

#include <functional>
#include <string>
#include <stdexcept>
#include <iostream>

template<typename K, typename V = K>
class MyRedBlackTree {
private:
	enum class Color { RED, BLACK };

	struct Node {
		K key;
		V val;
		Node* parent;
		Node* left;
		Node* right;
		Color color;

		Node(const K& _key, const V& _val, Color _color = Color::RED,
			Node* p = nullptr, Node* l = nullptr, Node* r = nullptr)
			: key(_key), val(_val), color(_color), parent(p), left(l), right(r) {
		}

	};

	Node* _root;
	Node* _nil;
	size_t _size;

public:
	using Self = MyRedBlackTree<K, V>;

	MyRedBlackTree() : _root(nullptr), _nil(nullptr), _size(0) {
		_nil = new Node(K{}, V{}, Color::BLACK);
		_nil->left = _nil;
		_nil->right = _nil;
		_nil->parent = _nil;
		_root = _nil;
	}

	MyRedBlackTree(const Self&) = delete;
	Self& operator=(const Self&) = delete;

	~MyRedBlackTree() {
		destroyTree(_root);

		delete _nil;

		_nil = nullptr;
		_root = nullptr;
		_size = 0;
	}

	bool insert(const K& key, const V& val) {
		Node* z = new Node(key, val);
		z->left = _nil;
		z->right = _nil;

		Node* y = _nil;
		Node* x = _root;

		while (x != _nil) {
			y = x;

			if (z->key > x->key) {
				x = x->right;
			}
			else if (z->key < x->key) {
				x = x->left;
			}
			else {
				//相等的时候依据业务的需求来进行不同的处理
				delete z;
				return false;
			}

		}

		z->parent = y;
		if (y == _nil) {
			_root = z;
		}
		else if (z->key < y->key) {
			y->left = z;
		}
		else if (z->key > y->key) {
			y->right = z;
		}

		++_size;

		if (z->parent == _nil) {
			z->color = Color::BLACK;
		}
		else {
			insertFixup(z);
		}

		_root->color = Color::BLACK;
		return true;

	}

	bool remove(const K& key) {
		Node* z = searchNode(_root, key);
		if (z == _nil) return false;

		Node* y = z;
		Node* x = _nil;
		Color yOriginalColor = y->color;

		if (z->left == _nil) {
			x = z->right;
			transplant(z, z->right);
		}
		else if (z->right == _nil) {
			x = z->left;
			transplant(z, z->left);
		}
		else {
			y = minNum(z->right);
			yOriginalColor = y->color;
			x = y->right;

			if (y->parent == z) {
				x->parent = y;
			}
			else {
				transplant(y, y->right);
				y->right = z->right;
				y->right->parent = y;
			}

			transplant(z, y);
			y->left = z->left;
			y->left->parent = y;
			y->color = z->color;

		}

		delete z;
		--_size;

		if (yOriginalColor == Color::BLACK) {
			removeFixup(x);
		}

		_root->parent = _nil;

		return true;

	}

	V* minValue() {
		Node* node = minNum(_root);
		return (node == _nil) ? nullptr : &node->val;
	}

	V* maxValue() {
		Node* node = maxNum(_root);
		return (node == _nil) ? nullptr : &node->val;
	}

	V* find(const K& key) {
		Node* node = searchNode(_root, key);
		return (node == _nil) ? nullptr : &node->val;
	}

	const V* find(const K& key) const {
		Node* node = searchNode(_root, key);
		return (node == _nil) ? nullptr : &node->val;
	}

	V& operator[](const K& key) {
		Node* node = searchNode(_root, key);

		if (node == _nil) {
			insert(key, V{});
			node = searchNode(_root, key);
		}

		return node->val;
	}

	size_t size() const { return _size; }
	bool isEmpty() const { return _size == 0; }

	void clear() {
		destroyTree(_root);

		_root = _nil;
		_nil->parent = _nil;
		_nil->left = _nil;
		_nil->right = _nil;
		_size = 0;
	}

private:
	bool isRed(const Node* node) const {
		return (node != _nil) ? node->color == Color::RED : false;
	}

	bool isBlack(const Node* node) const {
		return (node != _nil) ? node->color == Color::BLACK : true;
	}

	Color nodeColor(const Node* node) const {
		return (node == _nil) ? Color::BLACK : node->color;
	}

	Node* minNum(Node* node) const {
		if (node == _nil) return _nil;

		while (node->left != _nil) {
			node = node->left;
		}

		return node;
	}

	Node* maxNum(Node* node) const {
		if (node == _nil) return _nil;

		while (node->right != _nil) {
			node = node->right;
		}

		return node;
	}

	Node* searchNode(Node* node, const K& key) const {
		if (node == _nil) return _nil;

		while (node != _nil) {
			if (key > node->key) {
				node = node->right;
			}
			else if (key < node->key) {
				node = node->left;
			}
			else {
				return node;
			}
		}

		return _nil;
	}

	void rotateLeft(Node* node) {
		if (node == _nil || node->right == _nil) return;

		Node* rchild = node->right;

		node->right = rchild->left;
		if (rchild->left != _nil) {
			rchild->left->parent = node;
		}

		rchild->parent = node->parent;
		if (node->parent == _nil) {
			_root = rchild;
		}
		else if (node->parent->left == node) {
			node->parent->left = rchild;
		}
		else if (node->parent->right == node) {
			node->parent->right = rchild;
		}

		rchild->left = node;
		node->parent = rchild;

	}

	void rotateRight(Node* node) {
		if (node == _nil || node->left == _nil) return;

		Node* lchild = node->left;

		node->left = lchild->right;
		if (lchild->right != _nil) {
			lchild->right->parent = node;
		}

		lchild->parent = node->parent;
		if (node->parent == _nil) {
			_root = lchild;
		}
		else if (node->parent->left == node) {
			node->parent->left = lchild;
		}
		else if (node->parent->right == node) {
			node->parent->right = lchild;
		}

		lchild->right = node;
		node->parent = lchild;

	}

	void insertFixup(Node* z) {
		while (z != _root && isRed(z->parent)) {
			Node* zp = z->parent;
			Node* zpp = zp->parent;

			if (zp == zpp->left) {
				Node* zu = zpp->right;

				if (isRed(zu)) {
					zp->color = Color::BLACK;
					zu->color = Color::BLACK;
					zpp->color = Color::RED;
					z = zpp;
				}
				else {
					if (z == zp->right) {
						z = zp;
						rotateLeft(z);
						zp = z->parent;
						zpp = zp->parent;
					}

					zp->color = Color::BLACK;
					zpp->color = Color::RED;
					rotateRight(zpp);

				}

			}
			else {
				Node* zu = zpp->left;

				if (isRed(zu)) {
					zp->color = Color::BLACK;
					zu->color = Color::BLACK;
					zpp->color = Color::RED;
					z = zpp;
				}
				else {
					if (z == zp->left) {
						z = zp;
						rotateRight(z);
						zp = z->parent;
						zpp = zp->parent;
					}

					zp->color = Color::BLACK;
					zpp->color = Color::RED;
					rotateLeft(zpp);

				}

			}

		}

		_root->color = Color::BLACK;
	}

	void transplant(Node* u, Node* v) {
		if (u->parent == _nil) {
			_root = v;
		}
		else if (u->parent->left == u) {
			u->parent->left = v;
		}
		else if (u->parent->right == u) {
			u->parent->right = v;
		}

		v->parent = u->parent;

	}

	void removeFixup(Node* x) {
		while (x != _root && isBlack(x)) {
			Node* xp = x->parent;

			if (xp->left == x) {
				Node* w = xp->right;

				if (isRed(w)) {
					w->color = Color::BLACK;
					xp->color = Color::RED;
					rotateLeft(xp);
					w = xp->right;
				}

				if (isBlack(w->left) && isBlack(w->right)) {
					w->color = Color::RED;
					x = xp;
				}
				else {
					if (isBlack(w->right)) {
						w->left->color = Color::BLACK;
						w->color = Color::RED;
						rotateRight(w);
						w = xp->right;
					}

					w->color = nodeColor(xp);
					xp->color = Color::BLACK;
					w->right->color = Color::BLACK;
					rotateLeft(xp);
					x = _root;

				}

			}
			else {
				Node* w = xp->left;

				if (isRed(w)) {
					w->color = Color::BLACK;
					xp->color = Color::RED;
					rotateRight(xp);
					w = xp->left;
				}

				if (isBlack(w->left) && isBlack(w->right)) {
					w->color = Color::RED;
					x = xp;
				}
				else {
					if (isBlack(w->left)) {
						w->right->color = Color::BLACK;
						w->color = Color::RED;
						rotateLeft(w);
						w = xp->left;
					}

					w->color = nodeColor(xp);
					xp->color = Color::BLACK;
					w->left->color = Color::BLACK;
					rotateRight(xp);
					x = _root;

				}

			}

		}
		x->color = Color::BLACK;

	}

	void destroyTree(Node* node) {
		if (node == _nil) return;

		destroyTree(node->left);
		destroyTree(node->right);

		delete node;
	}

};

#endif // !MYREDBLACKTREE_HPP
